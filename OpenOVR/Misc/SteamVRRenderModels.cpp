#include "stdafx.h"

#include "SteamVRRenderModels.h"

#include "Config.h"

#include "Misc/AppManifest.h"
#include "Misc/JsonFile.h"
#include "Misc/Numbers.h"
#include "Misc/SteamVRPaths.h"
#include "Misc/lodepng.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <tuple>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace fs = std::filesystem;

namespace {

// Files larger than these aren't a render model's: refused before they're read
constexpr uintmax_t kMaxJsonBytes = 4 * 1024 * 1024;
constexpr uintmax_t kMaxObjBytes = 64 * 1024 * 1024;
constexpr uintmax_t kMaxPngBytes = 64 * 1024 * 1024;
constexpr uint32_t kMaxTextureSide = 8192;
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
	try {
		fs::path folder = SteamVRRenderModelFolder(steamvr, name);
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

bool LoadObj(const fs::path& obj, std::vector<RmVertex>& verts, std::vector<uint16_t>& indices, std::string* diffuse)
{
	if (diffuse)
		diffuse->clear();
	try {
		std::optional<std::string> text = ReadFile(obj, kMaxObjBytes);
		if (!text)
			return false;

		std::vector<glm::vec3> positions, normals;
		std::vector<glm::vec2> uvs;
		std::vector<RmVertex> newVerts;
		std::vector<uint16_t> newIndices;
		std::map<std::tuple<long, long, long>, uint16_t> corners;
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

		std::string_view all = *text;
		size_t lineStart = 0;
		// One pass over the lines: bounded by the file's size
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
					auto key = std::make_tuple(vi, ti, ni);
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
				if (material.empty() && newIndices.empty())
					material = std::string(Rest(line, 6));
			}
		}
		if (newIndices.empty())
			return false;

		if (diffuse) {
			for (const std::string& lib : libraries) {
				if (!IsInsideFolder(lib))
					continue;
				std::string file = DiffuseOf(obj.parent_path() / PathFromUtf8(lib), material);
				if (!file.empty() && IsInsideFolder(file)) {
					*diffuse = file;
					break;
				}
			}
		}
		verts.insert(verts.end(), newVerts.begin(), newVerts.end());
		indices.insert(indices.end(), newIndices.begin(), newIndices.end());
		return true;
	} catch (const std::exception& e) {
		OOVR_LOGF("Render model file %s can't be read: %s", PathToUtf8(obj).c_str(), e.what());
		return false;
	}
}

std::optional<RmMesh> LoadRenderModelMesh(const RmModel& model, const std::string& component)
{
	RmMesh mesh;
	auto add = [&](const fs::path& obj) {
		std::string diffuse;
		if (!LoadObj(obj, mesh.vertices, mesh.indices, &diffuse)) {
			OOVR_LOGF("Render model %s: %s can't be read", model.name.c_str(), PathToUtf8(obj).c_str());
			return;
		}
		if (mesh.diffuse.empty() && !diffuse.empty())
			mesh.diffuse = obj.parent_path() / PathFromUtf8(diffuse);
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
	return mesh;
}

std::optional<RmTexture> LoadRenderModelTexture(const fs::path& file)
{
	std::optional<std::string> data = ReadFile(file, kMaxPngBytes);
	if (!data) {
		OOVR_LOGF("Render model texture %s can't be read", PathToUtf8(file).c_str());
		return std::nullopt;
	}
	const unsigned char* bytes = (const unsigned char*)data->data();
	unsigned w = 0, h = 0;
	LodePNGState state;
	lodepng_state_init(&state);
	unsigned error = lodepng_inspect(&w, &h, &state, bytes, data->size());
	lodepng_state_cleanup(&state);
	if (error) {
		OOVR_LOGF("Render model texture %s isn't a PNG (%s): the model has no texture", PathToUtf8(file).c_str(), lodepng_error_text(error));
		return std::nullopt;
	}
	if (w == 0 || h == 0 || w > kMaxTextureSide || h > kMaxTextureSide) {
		OOVR_LOGF("Render model texture %s is %ux%u, larger than %u on a side: the model has no texture", PathToUtf8(file).c_str(), w, h, kMaxTextureSide);
		return std::nullopt;
	}
	RmTexture tex;
	try {
		std::vector<unsigned char> rgba;
		error = lodepng::decode(rgba, w, h, bytes, data->size());
		if (error || rgba.size() != (size_t)w * h * 4) {
			OOVR_LOGF("Render model texture %s can't be decoded (%s)", PathToUtf8(file).c_str(), error ? lodepng_error_text(error) : "wrong size");
			return std::nullopt;
		}
		tex.width = w;
		tex.height = h;
		tex.rgba = std::move(rgba);
	} catch (const std::exception& e) {
		OOVR_LOGF("Render model texture %s can't be decoded: %s", PathToUtf8(file).c_str(), e.what());
		return std::nullopt;
	}
	return tex;
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
