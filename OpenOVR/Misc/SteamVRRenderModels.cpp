#include "stdafx.h"

#include "SteamVRRenderModels.h"

#include "Config.h"

#include "Misc/AppManifest.h"
#include "Misc/JsonFile.h"
#include "Misc/Numbers.h"
#include "Misc/StbJpeg.h"
#include "Misc/SteamVRPaths.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <tuple>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#ifndef _WIN32
#include <dlfcn.h>
#endif

namespace fs = std::filesystem;

namespace {

// Files larger than these aren't a render model's: refused before they're read
constexpr uintmax_t kMaxJsonBytes = 4 * 1024 * 1024;
constexpr uintmax_t kMaxObjBytes = 64 * 1024 * 1024;
// More folders than any SteamVR install has: listing stops there
constexpr int kMaxListed = 4096;

bool IsDirectory(const fs::path& p)
{
	std::error_code ec;
	return fs::is_directory(p, ec);
}

bool IsFile(const fs::path& p)
{
	std::error_code ec;
	return fs::is_regular_file(p, ec);
}

// The whole file, if it exists and is no larger than limit
std::optional<std::string> ReadFile(const fs::path& p, uintmax_t limit)
{
	std::error_code ec;
	uintmax_t size = fs::file_size(p, ec);
	if (ec || size > limit)
		return std::nullopt;
	std::ifstream in(p, std::ios::binary);
	if (!in)
		return std::nullopt;
	std::string data((size_t)size, '\0');
	in.read(data.data(), (std::streamsize)size);
	data.resize((size_t)in.gcount());
	return data;
}

// One part of a model name: a folder name that can't leave the folder it's in
bool IsPlainName(const std::string& s)
{
	if (s.empty() || s == "." || s == "..")
		return false;
	for (char c : s) {
		if (c == '/' || c == '\\' || c == ':' || (unsigned char)c < 0x20)
			return false;
	}
	return true;
}

// A file named in a model's files: relative, inside the model's folder
bool IsInsideFolder(const std::string& file)
{
	if (file.empty())
		return false;
	fs::path p = PathFromUtf8(file);
	if (p.empty() || p.is_absolute() || p.has_root_name() || p.has_root_directory())
		return false;
	for (const fs::path& part : p)
		if (part == "..")
			return false;
	return true;
}

// The model's own file name: the name, without a driver's "{<driver>}"
std::string FileStem(const std::string& name)
{
	size_t close = name.size() > 0 && name[0] == '{' ? name.find('}') : std::string::npos;
	return close == std::string::npos ? name : name.substr(close + 1);
}

bool ReadVec3(const Json::Value& v, glm::vec3& out)
{
	if (!v.isArray() || v.size() != 3)
		return false;
	for (Json::ArrayIndex i = 0; i < 3; i++) {
		if (!v[i].isNumeric())
			return false;
		out[(int)i] = v[i].asFloat();
	}
	return true;
}

glm::vec3 Vec3Or(const Json::Value& obj, const char* key, glm::vec3 fallback)
{
	glm::vec3 v(0);
	return obj.isObject() && ReadVec3(obj[key], v) ? v : fallback;
}

// A [from, to] pair; 0, 0 without one
glm::vec2 Range(const Json::Value& obj, const char* key)
{
	if (!obj.isObject())
		return glm::vec2(0);
	const Json::Value& v = obj[key];
	if (!v.isArray() || v.size() != 2 || !v[0].isNumeric() || !v[1].isNumeric())
		return glm::vec2(0);
	return glm::vec2(v[0].asFloat(), v[1].asFloat());
}

float Lerp(glm::vec2 range, float t)
{
	return range.x + (range.y - range.x) * t;
}

// rotate_xyz: degrees about X, then Y, then Z
glm::quat EulerXyz(glm::vec3 degrees)
{
	glm::vec3 r = glm::radians(degrees);
	return glm::angleAxis(r.z, glm::vec3(0, 0, 1)) * glm::angleAxis(r.y, glm::vec3(0, 1, 0)) * glm::angleAxis(r.x, glm::vec3(1, 0, 0));
}

// A rigid transform: p' = rot * p + pos
struct Rigid {
	glm::quat rot = glm::quat(1, 0, 0, 0);
	glm::vec3 pos = glm::vec3(0);
};

// a after b
Rigid Then(const Rigid& a, const Rigid& b)
{
	return Rigid{ a.rot * b.rot, a.rot * b.pos + a.pos };
}

Rigid RotateAbout(glm::vec3 point, glm::quat rot)
{
	return Rigid{ rot, point - rot * point };
}

XrPosef ToPose(const Rigid& r)
{
	glm::quat q = glm::normalize(r.rot);
	XrPosef p;
	p.orientation = { q.x, q.y, q.z, q.w };
	p.position = { r.pos.x, r.pos.y, r.pos.z };
	return p;
}

// A component's entry; false if it isn't one
bool ReadComponent(const std::string& name, const Json::Value& v, RmComponent& out)
{
	if (!v.isObject())
		return false;
	out.name = name;
	const Json::Value& file = v["filename"];
	if (!file.isNull()) {
		if (!file.isString())
			return false;
		out.objFile = file.asString();
	}
	glm::vec3 origin(0), rotate(0);
	const Json::Value& local = v["component_local"];
	if (!local.isNull()) {
		if (!local.isObject())
			return false;
		if (!local["origin"].isNull() && !ReadVec3(local["origin"], origin))
			return false;
		if (!local["rotate_xyz"].isNull() && !ReadVec3(local["rotate_xyz"], rotate))
			return false;
	}
	out.local = ToPose(Rigid{ EulerXyz(rotate), origin });
	const Json::Value& motion = v["motion"];
	if (!motion.isNull()) {
		if (!motion.isObject())
			return false;
		const Json::Value& type = motion["type"];
		if (!type.isNull() && !type.isString())
			return false;
		out.motionType = type.isString() ? type.asString() : "";
		if (out.motionType == "none")
			out.motionType.clear();
		out.motion = motion;
	}
	const Json::Value& visibility = v["visibility"];
	if (!visibility.isNull()) {
		if (!visibility.isObject())
			return false;
		out.visibility = visibility;
	}
	return true;
}

// A whole number at the start of s (after spaces), and where it ended
bool ReadInt(std::string_view s, size_t& pos, long& out)
{
	while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t'))
		pos++;
	auto [end, ec] = std::from_chars(s.data() + pos, s.data() + s.size(), out);
	if (ec != std::errc())
		return false;
	pos = (size_t)(end - s.data());
	return true;
}

// An OBJ index (from 1, or negative from the end) as one from 0; -1 if it points nowhere
long Resolve(long index, size_t count)
{
	if (index > 0 && (size_t)index <= count)
		return index - 1;
	if (index < 0 && index >= -(long)count) // not -index: that overflows for the smallest long
		return (long)count + index;
	return -1;
}

// The rest of a line after its keyword, without the spaces around it
std::string_view Rest(std::string_view line, size_t keyword)
{
	std::string_view r = line.substr(std::min(keyword, line.size()));
	while (!r.empty() && (r.front() == ' ' || r.front() == '\t'))
		r.remove_prefix(1);
	while (!r.empty() && (r.back() == ' ' || r.back() == '\t' || r.back() == '\r'))
		r.remove_suffix(1);
	return r;
}

// A material library's map_Kd for the material, relative to its folder; "" for none
std::string DiffuseOf(const fs::path& mtl, const std::string& material)
{
	std::optional<std::string> text = ReadFile(mtl, kMaxObjBytes);
	if (!text)
		return {};
	std::istringstream lines(*text);
	std::string line, current;
	bool first = true;
	while (std::getline(lines, line)) {
		std::string_view l = Rest(line, 0);
		if (l.rfind("newmtl", 0) == 0) {
			current = std::string(Rest(l, 6));
			first = false;
		} else if (l.rfind("map_Kd", 0) == 0 && (current == material || (material.empty() && !first))) {
			std::string_view file = Rest(l, 6);
			// Options (-bm 1 and the like) come first: the file is then the last word
			if (!file.empty() && file.front() == '-') {
				size_t space = file.find_last_of(" \t");
				file = space == std::string_view::npos ? std::string_view() : file.substr(space + 1);
			}
			return std::string(file);
		}
	}
	return {};
}

vr::HmdVector3_t V3(glm::vec3 v)
{
	return vr::HmdVector3_t{ { v.x, v.y, v.z } };
}

} // namespace

std::vector<std::string> ListSteamVRRenderModels(const fs::path& steamvr)
{
	std::vector<std::string> names;
	try {
		auto addFolders = [&](const fs::path& folder, const std::string& prefix) {
			std::error_code ec;
			int seen = 0;
			for (fs::directory_iterator it(folder, ec), end; !ec && it != end && seen < kMaxListed; it.increment(ec), seen++) {
				if (!IsDirectory(it->path()))
					continue;
				std::string name = PathToUtf8(it->path().filename());
				if (IsPlainName(name))
					names.push_back(prefix + name);
			}
		};
		addFolders(steamvr / "resources" / "rendermodels", "");
		std::error_code ec;
		int drivers = 0;
		for (fs::directory_iterator it(steamvr / "drivers", ec), end; !ec && it != end && drivers < kMaxListed; it.increment(ec), drivers++) {
			std::string driver = PathToUtf8(it->path().filename());
			if (IsPlainName(driver) && driver.find('}') == std::string::npos)
				addFolders(it->path() / "resources" / "rendermodels", "{" + driver + "}");
		}
	} catch (const std::exception& e) {
		OOVR_LOGF("Listing SteamVR's render models failed: %s", e.what());
	}
	std::sort(names.begin(), names.end());
	names.erase(std::unique(names.begin(), names.end()), names.end());
	return names;
}

fs::path SteamVRRenderModelFolder(const fs::path& steamvr, const std::string& name)
{
	if (!name.empty() && name[0] == '{') {
		size_t close = name.find('}');
		if (close == std::string::npos)
			return {};
		std::string driver = name.substr(1, close - 1), model = name.substr(close + 1);
		if (!IsPlainName(driver) || !IsPlainName(model))
			return {};
		fs::path d = PathFromUtf8(driver), m = PathFromUtf8(model);
		if (d.empty() || m.empty())
			return {};
		return steamvr / "drivers" / d / "resources" / "rendermodels" / m;
	}
	if (!IsPlainName(name))
		return {};
	fs::path m = PathFromUtf8(name);
	if (m.empty())
		return {};
	return steamvr / "resources" / "rendermodels" / m;
}

std::optional<RmModel> LoadSteamVRRenderModel(const fs::path& steamvr, const std::string& name)
{
	fs::path folder;
	try {
		folder = SteamVRRenderModelFolder(steamvr, name);
	} catch (const std::exception& e) {
		OOVR_LOGF("Render model %s can't be read: %s", name.c_str(), e.what());
		return std::nullopt;
	}
	return LoadRenderModelFolder(folder, name);
}

std::optional<RmModel> LoadRenderModelFolder(const fs::path& folder, const std::string& name)
{
	try {
		if (folder.empty() || !IsDirectory(folder))
			return std::nullopt;
		fs::path stem = PathFromUtf8(FileStem(name));
		fs::path json = folder / stem;
		json += ".json";
		fs::path obj = folder / stem;
		obj += ".obj";

		RmModel model;
		model.name = name;
		model.folder = folder;
		if (!IsFile(json)) {
			// Most of SteamVR's models are one OBJ, with no components
			if (!IsFile(obj))
				return std::nullopt;
			return model;
		}

		std::error_code ec;
		uintmax_t size = fs::file_size(json, ec);
		if (ec || size > kMaxJsonBytes) {
			OOVR_LOGF("Render model %s: %s is too large to be a model's", name.c_str(), PathToUtf8(json).c_str());
			return std::nullopt;
		}
		Json::Value root;
		std::string error;
		if (!ReadJsonFileLenient(json, root, error) || !root.isObject()) {
			OOVR_LOGF("Render model %s can't be read, the built-in model is used: %s", name.c_str(),
			    error.empty() ? "not a JSON object" : error.c_str());
			return std::nullopt;
		}
		const Json::Value& thumbnail = root["thumbnail"];
		if (thumbnail.isString() && IsInsideFolder(thumbnail.asString()))
			model.thumbnail = thumbnail.asString();
		const Json::Value& components = root["components"];
		if (!components.isNull() && !components.isObject()) {
			OOVR_LOGF("Render model %s: its components aren't an object, the built-in model is used", name.c_str());
			return std::nullopt;
		}
		if (components.isObject()) {
			for (const std::string& key : components.getMemberNames()) {
				RmComponent c;
				if (!ReadComponent(key, components[key], c)) {
					OOVR_LOGF("Render model %s: component %s isn't one, the built-in model is used", name.c_str(), key.c_str());
					return std::nullopt;
				}
				if (!c.objFile.empty() && !IsInsideFolder(c.objFile)) {
					OOVR_LOGF("Render model %s: component %s's file is outside its folder, not drawn", name.c_str(), key.c_str());
					c.objFile.clear();
				}
				model.components.push_back(std::move(c));
			}
		}
		return model;
	} catch (const std::exception& e) {
		OOVR_LOGF("Render model %s can't be read: %s", name.c_str(), e.what());
		return std::nullopt;
	}
}

bool ParseObj(std::string_view all, std::vector<RmVertex>& verts, std::vector<uint16_t>& indices, ObjMaterials* materials)
{
	try {
		std::vector<glm::vec3> positions, normals;
		std::vector<glm::vec2> uvs;
		std::vector<RmVertex> newVerts;
		std::vector<uint16_t> newIndices;
		std::vector<std::string> newVertexMaterials;
		// One vertex per corner and material: a corner of two materials' faces is two vertices, as their textures differ
		std::map<std::tuple<long, long, long, std::string>, uint16_t> corners;
		std::vector<std::string> libraries;
		std::string material;
		size_t base = verts.size();

		auto floats = [](std::string_view rest, float* out, int n) {
			size_t pos = 0;
			for (int i = 0; i < n; i++) {
				size_t used = 0;
				std::optional<float> f = Numbers::ParseFloat(rest.substr(pos), &used);
				if (!f || !std::isfinite(*f))
					return false;
				out[i] = *f;
				pos += used;
			}
			return true;
		};

		size_t lineStart = 0;
		// One pass over the lines: bounded by the text's size
		while (lineStart < all.size()) {
			size_t lineEnd = all.find('\n', lineStart);
			if (lineEnd == std::string_view::npos)
				lineEnd = all.size();
			std::string_view line = Rest(all.substr(lineStart, lineEnd - lineStart), 0);
			lineStart = lineEnd + 1;
			if (line.empty() || line[0] == '#')
				continue;

			if (line.rfind("v ", 0) == 0 || line.rfind("v\t", 0) == 0) {
				float p[3] = {};
				if (!floats(Rest(line, 1), p, 3))
					return false;
				positions.emplace_back(p[0], p[1], p[2]);
			} else if (line.rfind("vt ", 0) == 0 || line.rfind("vt\t", 0) == 0) {
				float t[2] = {};
				if (!floats(Rest(line, 2), t, 2))
					return false;
				uvs.emplace_back(t[0], t[1]);
			} else if (line.rfind("vn ", 0) == 0 || line.rfind("vn\t", 0) == 0) {
				float n[3] = {};
				if (!floats(Rest(line, 2), n, 3))
					return false;
				normals.emplace_back(n[0], n[1], n[2]);
			} else if (line.rfind("f ", 0) == 0 || line.rfind("f\t", 0) == 0) {
				std::string_view rest = Rest(line, 1);
				std::vector<uint16_t> face;
				size_t pos = 0;
				// Each corner: v, v/t, v//n or v/t/n; at most one per character of the line
				while (pos < rest.size()) {
					long v = 0, t = 0, n = 0;
					if (!ReadInt(rest, pos, v))
						return false;
					if (pos < rest.size() && rest[pos] == '/') {
						pos++;
						if (pos < rest.size() && rest[pos] != '/' && !ReadInt(rest, pos, t))
							return false;
						if (pos < rest.size() && rest[pos] == '/') {
							pos++;
							if (!ReadInt(rest, pos, n))
								return false;
						}
					}
					long vi = Resolve(v, positions.size());
					long ti = t ? Resolve(t, uvs.size()) : -2;
					long ni = n ? Resolve(n, normals.size()) : -2;
					if (vi < 0 || ti == -1 || ni == -1)
						return false;
					auto key = std::make_tuple(vi, ti, ni, material);
					auto found = corners.find(key);
					if (found == corners.end()) {
						if (base + newVerts.size() >= 65535)
							return false;
						RmVertex vert{};
						vert.vPosition = V3(positions[(size_t)vi]);
						if (ni >= 0)
							vert.vNormal = V3(normals[(size_t)ni]);
						if (ti >= 0) {
							vert.rfTextureCoord[0] = uvs[(size_t)ti].x;
							vert.rfTextureCoord[1] = 1.0f - uvs[(size_t)ti].y;
						}
						found = corners.emplace(key, (uint16_t)(base + newVerts.size())).first;
						newVerts.push_back(vert);
						newVertexMaterials.push_back(material);
					}
					face.push_back(found->second);
					while (pos < rest.size() && (rest[pos] == ' ' || rest[pos] == '\t'))
						pos++;
				}
				if (face.size() < 3)
					return false;
				for (size_t i = 1; i + 1 < face.size(); i++) {
					newIndices.push_back(face[0]);
					newIndices.push_back(face[i]);
					newIndices.push_back(face[i + 1]);
				}
			} else if (line.rfind("mtllib", 0) == 0) {
				libraries.emplace_back(Rest(line, 6));
			} else if (line.rfind("usemtl", 0) == 0) {
				material = std::string(Rest(line, 6));
			}
		}
		if (newIndices.empty())
			return false;

		if (materials) {
			materials->libraries.insert(materials->libraries.end(), libraries.begin(), libraries.end());
			materials->vertexMaterials.insert(materials->vertexMaterials.end(), newVertexMaterials.begin(), newVertexMaterials.end());
		}
		verts.insert(verts.end(), newVerts.begin(), newVerts.end());
		indices.insert(indices.end(), newIndices.begin(), newIndices.end());
		return true;
	} catch (const std::exception& e) {
		OOVR_LOGF("Render model text can't be read: %s", e.what());
		return false;
	}
}

namespace {
// Each vertex's texture of an OBJ file just read: its material's map_Kd in the OBJ's libraries, as a path (empty for
// none). The libraries are read once for each material.
std::vector<fs::path> VertexTextures(const fs::path& obj, const ObjMaterials& materials)
{
	std::map<std::string, fs::path> byMaterial;
	std::vector<fs::path> textures;
	textures.reserve(materials.vertexMaterials.size());
	for (const std::string& material : materials.vertexMaterials) {
		auto it = byMaterial.find(material);
		if (it == byMaterial.end()) {
			fs::path file;
			for (const std::string& lib : materials.libraries) {
				if (!IsInsideFolder(lib))
					continue;
				std::string diffuse = DiffuseOf(obj.parent_path() / PathFromUtf8(lib), material);
				if (!diffuse.empty() && IsInsideFolder(diffuse)) {
					file = obj.parent_path() / PathFromUtf8(diffuse);
					break;
				}
			}
			it = byMaterial.emplace(material, file).first;
		}
		textures.push_back(it->second);
	}
	return textures;
}
} // namespace

bool LoadObj(const fs::path& obj, std::vector<RmVertex>& verts, std::vector<uint16_t>& indices, std::string* diffuse)
{
	if (diffuse)
		diffuse->clear();
	std::optional<std::string> text = ReadFile(obj, kMaxObjBytes);
	if (!text)
		return false;
	ObjMaterials materials;
	if (!ParseObj(*text, verts, indices, &materials)) {
		OOVR_LOGF("Render model file %s can't be read", PathToUtf8(obj).c_str());
		return false;
	}
	if (diffuse) {
		// The first material used that has a texture
		for (const fs::path& file : VertexTextures(obj, materials)) {
			if (!file.empty()) {
				*diffuse = PathToUtf8(file.lexically_relative(obj.parent_path()));
				break;
			}
		}
	}
	return true;
}

namespace {
// Adds an OBJ file's triangles to the mesh, each vertex's texture to vertexTextures
bool AddObj(const fs::path& obj, RmMesh& mesh, std::vector<fs::path>& vertexTextures)
{
	std::optional<std::string> text = ReadFile(obj, kMaxObjBytes);
	ObjMaterials materials;
	if (!text || !ParseObj(*text, mesh.vertices, mesh.indices, &materials))
		return false;
	std::vector<fs::path> textures = VertexTextures(obj, materials);
	vertexTextures.insert(vertexTextures.end(), textures.begin(), textures.end());
	return true;
}

// The mesh's textures from each vertex's: one is the mesh's diffuse; several are its atlas, each a band of it from the
// top in the order first used, with the vertices' v moved into their texture's band. A vertex with no texture keeps its
// UV in the first.
void SetTextures(RmMesh& mesh, const std::vector<fs::path>& vertexTextures)
{
	for (const fs::path& file : vertexTextures)
		if (!file.empty() && std::find(mesh.textures.begin(), mesh.textures.end(), file) == mesh.textures.end())
			mesh.textures.push_back(file);
	mesh.diffuse = mesh.textures.empty() ? fs::path() : mesh.textures.front();
	if (mesh.textures.size() < 2)
		return;
	const float bands = (float)mesh.textures.size();
	for (size_t i = 0; i < mesh.vertices.size() && i < vertexTextures.size(); i++) {
		size_t band = 0;
		if (!vertexTextures[i].empty())
			band = (size_t)(std::find(mesh.textures.begin(), mesh.textures.end(), vertexTextures[i]) - mesh.textures.begin());
		float& v = mesh.vertices[i].rfTextureCoord[1];
		v = ((float)band + std::clamp(v, 0.0f, 1.0f)) / bands;
	}
}
} // namespace

std::optional<RmMesh> LoadObjMesh(const fs::path& obj)
{
	RmMesh mesh;
	std::vector<fs::path> vertexTextures;
	if (!AddObj(obj, mesh, vertexTextures)) {
		OOVR_LOGF("Render model file %s can't be read", PathToUtf8(obj).c_str());
		return std::nullopt;
	}
	SetTextures(mesh, vertexTextures);
	return mesh;
}

std::optional<RmMesh> LoadRenderModelMesh(const RmModel& model, const std::string& component)
{
	RmMesh mesh;
	std::vector<fs::path> vertexTextures;
	auto add = [&](const fs::path& obj) {
		if (!AddObj(obj, mesh, vertexTextures))
			OOVR_LOGF("Render model %s: %s can't be read", model.name.c_str(), PathToUtf8(obj).c_str());
	};

	if (!component.empty()) {
		auto it = std::find_if(model.components.begin(), model.components.end(), [&](const RmComponent& c) { return c.name == component; });
		if (it == model.components.end() || it->objFile.empty())
			return std::nullopt;
		add(model.folder / PathFromUtf8(it->objFile));
	} else {
		fs::path whole = model.folder / PathFromUtf8(FileStem(model.name));
		whole += ".obj";
		if (IsFile(whole)) {
			add(whole);
		} else {
			// No OBJ of the whole model: its components as they are at rest
			for (const RmComponent& c : model.components)
				if (!c.objFile.empty() && ComponentVisible(c, false, false))
					add(model.folder / PathFromUtf8(c.objFile));
		}
	}
	if (mesh.indices.empty())
		return std::nullopt;
	SetTextures(mesh, vertexTextures);
	const XrPosef& s = model.space;
	if (s.orientation.w != 1 || s.position.x != 0 || s.position.y != 0 || s.position.z != 0) {
		glm::quat rot = glm::normalize(glm::quat(s.orientation.w, s.orientation.x, s.orientation.y, s.orientation.z));
		glm::vec3 pos(s.position.x, s.position.y, s.position.z);
		for (RmVertex& v : mesh.vertices) {
			glm::vec3 p = rot * glm::vec3(v.vPosition.v[0], v.vPosition.v[1], v.vPosition.v[2]) + pos;
			glm::vec3 n = rot * glm::vec3(v.vNormal.v[0], v.vNormal.v[1], v.vNormal.v[2]);
			v.vPosition = V3(p);
			v.vNormal = V3(n);
		}
	}
	return mesh;
}

namespace {
// A Truevision TGA image as RGBA8: true-colour (24 or 32 bits) or greyscale (8 bits), plain or run-length encoded, from
// either corner. nullopt (with why) for anything else.
std::optional<Image> DecodeTga(const std::string& bytes, std::string* why)
{
	auto fail = [&](const char* reason) -> std::optional<Image> {
		if (why)
			*why = reason;
		return std::nullopt;
	};
	if (bytes.size() < 18)
		return fail("it's too short to be a TGA image");
	auto u8 = [&](size_t i) { return (uint8_t)bytes[i]; };
	auto u16 = [&](size_t i) { return (uint32_t)(u8(i) | (u8(i + 1) << 8)); };
	const uint32_t idLength = u8(0), colourMapType = u8(1), type = u8(2);
	const uint32_t width = u16(12), height = u16(14), bits = u8(16), descriptor = u8(17);
	const bool rle = type == 10 || type == 11;
	const bool grey = type == 3 || type == 11;
	if (colourMapType != 0 || !(type == 2 || type == 3 || type == 10 || type == 11))
		return fail("it's a kind of TGA image that can't be read (only true-colour and greyscale ones can)");
	if ((grey && bits != 8) || (!grey && bits != 24 && bits != 32))
		return fail("its TGA pixel size can't be read");
	if (width == 0 || height == 0 || width > kMaxImageFileSide || height > kMaxImageFileSide)
		return fail("its TGA size is empty or too big");
	const size_t pixelBytes = bits / 8;
	size_t pos = 18 + idLength;
	// The least data that can hold that many pixels (run-length encoded: a packet of 128 per header and pixel)
	const size_t count = (size_t)width * height;
	const size_t least = rle ? (count + 127) / 128 * (1 + pixelBytes) : count * pixelBytes;
	if (pos > bytes.size() || bytes.size() - pos < least)
		return fail("its TGA data is cut short");
	Image image;
	image.width = width;
	image.height = height;
	image.rgba.resize((size_t)width * height * 4);
	auto put = [&](size_t index, size_t from) {
		uint8_t* out = &image.rgba[index * 4];
		if (grey) {
			out[0] = out[1] = out[2] = u8(from);
			out[3] = 255;
		} else {
			out[0] = u8(from + 2);
			out[1] = u8(from + 1);
			out[2] = u8(from);
			out[3] = pixelBytes == 4 ? u8(from + 3) : 255;
		}
	};
	size_t done = 0;
	while (done < count) {
		size_t run = 1;
		bool repeat = false;
		if (rle) {
			if (pos >= bytes.size())
				return fail("its TGA data is cut short");
			uint8_t header = u8(pos++);
			run = (size_t)(header & 0x7f) + 1;
			repeat = (header & 0x80) != 0;
		} else {
			run = count;
		}
		run = (std::min)(run, count - done);
		size_t needed = repeat ? pixelBytes : run * pixelBytes;
		if (pos + needed > bytes.size())
			return fail("its TGA data is cut short");
		for (size_t i = 0; i < run; i++)
			put(done + i, repeat ? pos : pos + i * pixelBytes);
		pos += needed;
		done += run;
	}
	// The rows start at the bottom unless the descriptor says top; an image's first row is its top
	if (!(descriptor & 0x20)) {
		const size_t row = (size_t)width * 4;
		for (uint32_t y = 0; y < height / 2; y++)
			std::swap_ranges(image.rgba.begin() + (std::ptrdiff_t)(y * row), image.rgba.begin() + (std::ptrdiff_t)((y + 1) * row),
			    image.rgba.begin() + (std::ptrdiff_t)((height - 1 - y) * row));
	}
	if (descriptor & 0x10) {
		for (uint32_t y = 0; y < height; y++)
			for (uint32_t x = 0; x < width / 2; x++)
				for (int c = 0; c < 4; c++)
					std::swap(image.rgba[((size_t)y * width + x) * 4 + c], image.rgba[((size_t)y * width + width - 1 - x) * 4 + c]);
	}
	return image;
}

// A file that starts as a JPEG does
bool IsJpeg(const fs::path& file)
{
	std::ifstream in(file, std::ios::binary);
	unsigned char magic[3] = {};
	return in.read((char*)magic, 3) && magic[0] == 0xFF && magic[1] == 0xD8 && magic[2] == 0xFF;
}

bool IsTga(const fs::path& file)
{
	std::string ext = PathToUtf8(file.extension());
	std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return ext == ".tga";
}
} // namespace

std::optional<Image> LoadRenderModelTexture(const fs::path& file)
{
	std::string why;
	std::optional<Image> image;
	if (IsTga(file)) {
		std::optional<std::string> bytes = ReadFile(file, kMaxImageFileBytes);
		image = bytes ? DecodeTga(*bytes, &why) : std::nullopt;
		if (!bytes)
			why = "it can't be read, or is too big";
	} else if (IsJpeg(file)) {
		std::optional<std::string> bytes = ReadFile(file, kMaxImageFileBytes);
		image = bytes ? DecodeJpeg(*bytes, &why) : std::nullopt;
		if (!bytes)
			why = "it can't be read, or is too big";
	} else {
		image = LoadImageFile(file, &why);
	}
	if (!image)
		OOVR_LOGF("Render model texture %s can't be used: %s; the model has no texture", PathToUtf8(file).c_str(), why.c_str());
	return image;
}

std::optional<Image> LoadRenderModelTextures(const std::vector<fs::path>& files)
{
	if (files.size() == 1)
		return LoadRenderModelTexture(files.front());
	// The textures are all held while their atlas is made: together no larger than one image may be
	std::vector<Image> images;
	uint32_t width = 0, height = 0;
	uint64_t bytes = 0;
	for (const fs::path& file : files) {
		std::optional<Image> image = LoadRenderModelTexture(file);
		if (!image)
			return std::nullopt;
		bytes += image->rgba.size();
		if (bytes > (uint64_t)kMaxImageFileSide * kMaxImageFileSide * 4) {
			OOVR_LOGF("Render model textures from %s on are too big together for one atlas; the model has no texture", PathToUtf8(file).c_str());
			return std::nullopt;
		}
		width = (std::max)(width, image->width);
		height = (std::max)(height, image->height);
		images.push_back(std::move(*image));
	}
	if (images.empty())
		return std::nullopt;
	// Each texture a band of the same size, the whole no taller than an image may be
	uint32_t band = (std::max)(1u, (std::min)(height, kMaxImageFileSide / (uint32_t)images.size()));
	Image atlas;
	atlas.width = width;
	atlas.height = band * (uint32_t)images.size();
	atlas.rgba.reserve((size_t)atlas.width * atlas.height * 4);
	for (const Image& image : images) {
		Image same = ResampleImage(image, width, band);
		atlas.rgba.insert(atlas.rgba.end(), same.rgba.begin(), same.rgba.end());
	}
	return atlas;
}

Image ResampleImage(const Image& image, uint32_t width, uint32_t height)
{
	Image out;
	if (image.Empty() || image.rgba.size() < (size_t)image.width * image.height * 4 || !width || !height)
		return out;
	out.width = width;
	out.height = height;
	out.rgba.resize((size_t)width * height * 4);
	for (uint32_t y = 0; y < height; y++) {
		uint32_t sy = (uint32_t)(((uint64_t)y * 2 + 1) * image.height / ((uint64_t)height * 2));
		for (uint32_t x = 0; x < width; x++) {
			uint32_t sx = (uint32_t)(((uint64_t)x * 2 + 1) * image.width / ((uint64_t)width * 2));
			memcpy(&out.rgba[((size_t)y * width + x) * 4], &image.rgba[((size_t)sy * image.width + sx) * 4], 4);
		}
	}
	return out;
}

std::vector<Image> BuildMipChain(const Image& base, uint32_t levels)
{
	std::vector<Image> chain{ base };
	if (base.Empty() || base.rgba.size() < (size_t)base.width * base.height * 4)
		return chain;
	while ((levels == 0 || chain.size() < levels) && (chain.back().width > 1 || chain.back().height > 1)) {
		const Image& up = chain.back();
		Image down;
		down.width = (std::max)(up.width / 2, 1u);
		down.height = (std::max)(up.height / 2, 1u);
		down.rgba.resize((size_t)down.width * down.height * 4);
		for (uint32_t y = 0; y < down.height; y++) {
			for (uint32_t x = 0; x < down.width; x++) {
				uint32_t x0 = (std::min)(x * 2, up.width - 1), x1 = (std::min)(x * 2 + 1, up.width - 1);
				uint32_t y0 = (std::min)(y * 2, up.height - 1), y1 = (std::min)(y * 2 + 1, up.height - 1);
				for (int c = 0; c < 4; c++) {
					uint32_t sum = up.rgba[((size_t)y0 * up.width + x0) * 4 + c] + up.rgba[((size_t)y0 * up.width + x1) * 4 + c]
					    + up.rgba[((size_t)y1 * up.width + x0) * 4 + c] + up.rgba[((size_t)y1 * up.width + x1) * 4 + c];
					down.rgba[((size_t)y * down.width + x) * 4 + c] = (uint8_t)((sum + 2) / 4);
				}
			}
		}
		chain.push_back(std::move(down));
	}
	return chain;
}

XrPosef ComponentPose(const RmComponent& c, float value, vr::HmdVector2_t axis)
{
	float v = std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
	float x = std::isfinite(axis.v[0]) ? std::clamp(axis.v[0], -1.0f, 1.0f) : 0.0f;
	float y = std::isfinite(axis.v[1]) ? std::clamp(axis.v[1], -1.0f, 1.0f) : 0.0f;
	const Json::Value& m = c.motion;
	Rigid r;

	if (c.motionType == "rotate") {
		glm::vec3 about = Vec3Or(m, "axis", glm::vec3(1, 0, 0));
		if (glm::length(about) > 1e-6f) {
			float degrees = Lerp(Range(m, "value_mapping"), v);
			r = RotateAbout(Vec3Or(m, "pivot", glm::vec3(0)), glm::angleAxis(glm::radians(degrees), glm::normalize(about)));
		}
	} else if (c.motionType == "translate") {
		r.pos = Vec3Or(m, "axis", glm::vec3(0)) * Lerp(Range(m, "value_mapping"), v);
	} else if (c.motionType == "trackpad" || c.motionType == "joystick") {
		bool pad = c.motionType == "trackpad";
		// The pad's (or stick's) frame: up is its Y, forward its -Z
		glm::quat frame = EulerXyz(Vec3Or(m, "rotate_xyz", glm::vec3(0)));
		// Right tilts about the frame's Z, forward about its X; a pad only tilts as far as it's pressed
		float aboutZ = Lerp(Range(m, pad ? "press_rotation_x" : "joystick_rotation_x"), (x + 1) / 2);
		float aboutX = Lerp(Range(m, pad ? "press_rotation_y" : "joystick_rotation_y"), (y + 1) / 2);
		if (pad) {
			aboutZ *= v;
			aboutX *= v;
		}
		glm::quat tilt = frame * glm::angleAxis(glm::radians(aboutZ), glm::vec3(0, 0, 1))
		    * glm::angleAxis(glm::radians(aboutX), glm::vec3(1, 0, 0)) * glm::inverse(frame);
		r = RotateAbout(Vec3Or(m, "center", glm::vec3(0)), tilt);
		glm::vec3 move = Vec3Or(m, "press_translate", glm::vec3(0)) * v;
		if (pad) {
			// The touch point: across the pad and along it, in its frame
			glm::vec3 touch(Lerp(Range(m, "touch_translate_x"), (x + 1) / 2), 0, Lerp(Range(m, "touch_translate_y"), (y + 1) / 2));
			move += frame * touch;
		}
		r = Then(Rigid{ glm::quat(1, 0, 0, 0), move }, r);
	}
	return ToPose(r);
}

namespace {
Rigid SpaceOf(const RmModel& model)
{
	const XrPosef& s = model.space;
	return Rigid{ glm::normalize(glm::quat(s.orientation.w, s.orientation.x, s.orientation.y, s.orientation.z)),
		glm::vec3(s.position.x, s.position.y, s.position.z) };
}

Rigid FromPose(const XrPosef& p)
{
	return Rigid{ glm::quat(p.orientation.w, p.orientation.x, p.orientation.y, p.orientation.z),
		glm::vec3(p.position.x, p.position.y, p.position.z) };
}

Rigid Inverse(const Rigid& r)
{
	glm::quat inv = glm::inverse(r.rot);
	return Rigid{ inv, -(inv * r.pos) };
}
} // namespace

XrPosef ModelComponentPose(const RmModel& model, const RmComponent& c, float value, vr::HmdVector2_t axis)
{
	// The motion is in the files' space: there, then back
	Rigid space = SpaceOf(model);
	return ToPose(Then(space, Then(FromPose(ComponentPose(c, value, axis)), Inverse(space))));
}

XrPosef ModelComponentLocal(const RmModel& model, const RmComponent& c)
{
	return ToPose(Then(SpaceOf(model), FromPose(c.local)));
}

bool ComponentVisible(const RmComponent& c, bool touched, bool pressed)
{
	const Json::Value& vis = c.visibility;
	if (!vis.isObject())
		return true;
	bool visible = vis["default"].isBool() ? vis["default"].asBool() : true;
	if (touched && vis["touch"].isBool())
		visible = vis["touch"].asBool();
	if (pressed && vis["press"].isBool())
		visible = vis["press"].asBool();
	return visible;
}

// The install's models, kept for the process: never freed, as a game's calls can come during static destruction
static std::mutex& ModelsLock()
{
	static std::mutex& lock = *new std::mutex;
	return lock;
}

static const std::optional<fs::path>& Install()
{
	// None unless the user wants SteamVR's models (steamvrRenderModels), or a test names its SteamVR folder
	const char* testDir = getenv("OC_TEST_STEAMVR_DIR");
	const bool wanted = oovr_global_configuration.SteamVRRenderModels() || (testDir && *testDir);
	static const std::optional<fs::path>& install = *new std::optional<fs::path>(wanted ? FindSteamVRInstall() : std::nullopt);
	return install;
}

std::shared_ptr<const RmModel> FindSteamVRRenderModel(const std::string& name)
{
	const std::optional<fs::path>& install = Install();
	if (!install || name.empty())
		return nullptr;
	// Kept: every name asked for, up to this many (a game asking for more is answered without the cache)
	constexpr size_t kMaxCached = 1024;
	static std::map<std::string, std::shared_ptr<const RmModel>>& cache = *new std::map<std::string, std::shared_ptr<const RmModel>>;
	{
		std::lock_guard<std::mutex> guard(ModelsLock());
		auto it = cache.find(name);
		if (it != cache.end())
			return it->second;
	}
	std::optional<RmModel> loaded = LoadSteamVRRenderModel(*install, name);
	std::shared_ptr<const RmModel> model = loaded ? std::make_shared<const RmModel>(std::move(*loaded)) : nullptr;
	std::lock_guard<std::mutex> guard(ModelsLock());
	if (cache.size() < kMaxCached)
		cache.emplace(name, model);
	return model;
}

const std::vector<std::string>& SteamVRRenderModelNames()
{
	static const std::vector<std::string>& names = *new std::vector<std::string>(
	    Install() ? ListSteamVRRenderModels(*Install()) : std::vector<std::string>());
	return names;
}

namespace {
// The folder of the library (or program) this code is in; empty if it can't be told
fs::path LibraryFolder()
{
#ifdef _WIN32
	HMODULE module = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	        (LPCWSTR)&LibraryFolder, &module))
		return {};
	std::wstring buffer(MAX_PATH, L'\0');
	for (;;) {
		DWORD n = GetModuleFileNameW(module, buffer.data(), (DWORD)buffer.size());
		if (n == 0)
			return {};
		if (n < buffer.size()) {
			buffer.resize(n);
			break;
		}
		if (buffer.size() >= 32768)
			return {};
		buffer.resize(buffer.size() * 2);
	}
	return fs::path(buffer).parent_path();
#else
	Dl_info info{};
	if (!dladdr((void*)&LibraryFolder, &info) || !info.dli_fname)
		return {};
	return fs::path(info.dli_fname).parent_path();
#endif
}
} // namespace

fs::path OwnRenderModelsFolder()
{
	// A test's folder where it names one; else the one beside the library, found once
	const char* testDir = getenv("OC_TEST_RENDERMODELS_DIR");
	if (testDir && *testDir)
		return PathFromUtf8(testDir);
	static const fs::path& folder = *[] {
		auto* found = new fs::path;
		try {
			fs::path lib = LibraryFolder();
			if (!lib.empty()) {
				lib = fs::absolute(lib);
				for (const fs::path& candidate : { lib / "rendermodels", lib.parent_path() / "rendermodels" }) {
					if (IsDirectory(candidate)) {
						*found = candidate;
						break;
					}
				}
			}
		} catch (const std::exception& e) {
			OOVR_LOGF("OpenLamina's controller models can't be found: %s", e.what());
		}
		if (found->empty())
			OOVR_LOG("OpenLamina's controller models aren't installed beside the library");
		return found;
	}();
	return folder;
}

namespace {
// "<profile>-<hand>": the profile's folder, or empty for a name that isn't one
std::string OwnModelProfile(const std::string& name)
{
	for (const char* hand : { "-left", "-right" }) {
		size_t n = strlen(hand);
		if (name.size() > n && name.compare(name.size() - n, n, hand) == 0) {
			std::string profile = name.substr(0, name.size() - n);
			return IsPlainName(profile) ? profile : std::string();
		}
	}
	return {};
}
} // namespace

std::shared_ptr<const RmModel> FindOwnRenderModel(const std::string& name, const XrPosef& space)
{
	const fs::path root = OwnRenderModelsFolder();
	std::string profile = OwnModelProfile(name);
	if (root.empty() || profile.empty() || !IsPlainName(name))
		return nullptr;
	// By folder, name and space: a test can name another folder, and the space is known once there's an instance
	using Key = std::tuple<fs::path, std::string, std::array<float, 7>>;
	static std::map<Key, std::shared_ptr<const RmModel>>& cache = *new std::map<Key, std::shared_ptr<const RmModel>>;
	const Key key(root, name,
	    std::array<float, 7>{ space.orientation.x, space.orientation.y, space.orientation.z, space.orientation.w,
	        space.position.x, space.position.y, space.position.z });
	{
		std::lock_guard<std::mutex> guard(ModelsLock());
		auto it = cache.find(key);
		if (it != cache.end())
			return it->second;
	}
	std::shared_ptr<const RmModel> model;
	try {
		std::optional<RmModel> loaded = LoadRenderModelFolder(root / PathFromUtf8(profile), name);
		if (loaded) {
			loaded->space = space;
			model = std::make_shared<const RmModel>(std::move(*loaded));
		}
	} catch (const std::exception& e) {
		OOVR_LOGF("Render model %s can't be read: %s", name.c_str(), e.what());
	}
	std::lock_guard<std::mutex> guard(ModelsLock());
	// Only names that are a model's folder get here: a few dozen at most
	cache.emplace(key, model);
	return model;
}

const std::vector<std::string>& OwnRenderModelNames()
{
	// By folder, each read once: a test can name another folder
	static std::map<fs::path, std::vector<std::string>>& byFolder = *new std::map<fs::path, std::vector<std::string>>;
	const fs::path root = OwnRenderModelsFolder();
	std::lock_guard<std::mutex> guard(ModelsLock());
	auto found = byFolder.find(root);
	if (found != byFolder.end())
		return found->second;
	std::vector<std::string>& all = byFolder[root];
	if (root.empty())
		return all;
	std::error_code ec;
	for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
		std::string profile = PathToUtf8(it->path().filename());
		if (!IsPlainName(profile))
			continue;
		for (const char* hand : { "-left", "-right" }) {
			fs::path json = it->path() / PathFromUtf8(profile + hand + ".json");
			if (IsFile(json))
				all.push_back(profile + hand);
		}
	}
	std::sort(all.begin(), all.end());
	return all;
}
