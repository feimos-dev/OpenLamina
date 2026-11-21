#include "stdafx.h"
#include "Misc/EnumNames.h"
#include <numbers>
#define BASE_IMPL
#include "BaseRenderModels.h"
#include "Misc/AppManifest.h"
#include "Misc/BuiltInResource.h"
#include "Misc/Config.h"
#include "Misc/SteamVRRenderModels.h"
#include "convert.h"
#include "generated/static_bases.gen.h"
#include "resources.h"

// Used for the hand offsets
#include "BaseCompositor.h"
#include "BaseInput.h"
#include "BaseSystem.h"
#include "Misc/Input/InteractionProfile.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtx/transform.hpp>

using glm::mat4;
using glm::quat;
using glm::vec3;
using glm::vec4;

#ifdef _MSC_VER
#pragma region structs
#endif

enum OOVR_EVRRenderModelError : int {
	VRRenderModelError_None = 0,
	VRRenderModelError_Loading = 100,
	VRRenderModelError_NotSupported = 200,
	VRRenderModelError_InvalidArg = 300,
	VRRenderModelError_InvalidModel = 301,
	VRRenderModelError_NoShapes = 302,
	VRRenderModelError_MultipleShapes = 303,
	VRRenderModelError_TooManyVertices = 304,
	VRRenderModelError_MultipleTextures = 305,
	VRRenderModelError_BufferTooSmall = 306,
	VRRenderModelError_NotEnoughNormals = 307,
	VRRenderModelError_NotEnoughTexCoords = 308,

	VRRenderModelError_InvalidTexture = 400,
};

struct OOVR_RenderModel_Vertex_t {
	vr::HmdVector3_t vPosition; // position in meters in device space
	vr::HmdVector3_t vNormal;
	float rfTextureCoord[2];
};

#if defined(__linux__) || defined(__APPLE__)
// This structure was originally defined mis-packed on Linux, preserved for
// compatibility.
#pragma pack(push, 4)
#endif

struct OOVR_RenderModel_t {
	const OOVR_RenderModel_Vertex_t* rVertexData; // Vertex data for the mesh
	uint32_t unVertexCount; // Number of vertices in the vertex data
	const uint16_t* rIndexData; // Indices into the vertex data for each triangle
	uint32_t unTriangleCount; // Number of triangles in the mesh. Index count is 3 * TriangleCount
	OOVR_TextureID_t diffuseTextureId; // Session unique texture identifier. Rendermodels which share the same texture will have the same id. <0 == texture not present
};

struct OOVR_RenderModel_TextureMap_t {
	uint16_t unWidth, unHeight; // width and height of the texture map in pixels
	const uint8_t* rubTextureMapData; // Map texture data. All textures are RGBA with 8 bits per channel per pixel. Data size is width * height * 4ub
	// Newer SDKs read these two after the data pointer; older ones stop before them
	int format; // 0 = RGBA8_SRGB
	uint16_t unMipLevels;
};

#if defined(__linux__) || defined(__APPLE__)
#pragma pack(pop)
#endif

typedef uint32_t VRComponentProperties;
enum OOVR_EVRComponentProperty {
	VRComponentProperty_IsStatic = (1 << 0),
	VRComponentProperty_IsVisible = (1 << 1),
	VRComponentProperty_IsTouched = (1 << 2),
	VRComponentProperty_IsPressed = (1 << 3),
	VRComponentProperty_IsScrolled = (1 << 4),
};

#ifdef _MSC_VER
#pragma endregion
#endif

typedef OOVR_RenderModel_t RenderModel_t;
typedef OOVR_EVRRenderModelError EVRRenderModelError;
typedef OOVR_RenderModel_TextureMap_t RenderModel_TextureMap_t;
typedef OOVR_TextureID_t TextureID_t;

// The render models OpenLamina builds in, each with one component named after it: every name LoadRenderModel_Async loads
static const char* const kOwnModelNames[] = {
	"renderLeftHand",
	"renderRightHand",
	"vive_tracker",
	"oculus_quest2_controller_left",
	"oculus_quest2_controller_right",
	"{indexcontroller}valve_controller_knu_1_0_left",
	"{indexcontroller}valve_controller_knu_1_0_right",
};

static bool IsOwnRenderModel(const char* name)
{
	for (const char* k : kOwnModelNames)
		if (strcmp(k, name) == 0)
			return true;
	return false;
}

// The own models and the headset's name, which is known but has nothing to draw
static bool IsKnownRenderModel(const char* name)
{
	return strcmp(name, "oculusHmdRenderModel") == 0 || IsOwnRenderModel(name);
}

// OpenLamina's own controller model of each interaction profile: the WebXR Input Profile it's made from
static const struct {
	const char* path;
	const char* model;
} kOwnControllerModels[] = {
	{ "/interaction_profiles/oculus/touch_controller", "oculus-touch-v3" },
	{ "/interaction_profiles/meta/touch_controller_plus", "meta-quest-touch-plus" },
	{ "/interaction_profiles/facebook/touch_controller_pro", "meta-quest-touch-pro" },
	{ "/interaction_profiles/valve/index_controller", "valve-index" },
	{ "/interaction_profiles/htc/vive_controller", "htc-vive" },
	{ "/interaction_profiles/microsoft/motion_controller", "microsoft-mixed-reality" },
	{ "/interaction_profiles/samsung/odyssey_controller", "microsoft-mixed-reality" },
	{ "/interaction_profiles/hp/mixed_reality_controller", "hp-mixed-reality" },
	{ "/interaction_profiles/htc/vive_cosmos_controller", "htc-vive-cosmos" },
	{ "/interaction_profiles/htc/vive_focus3_controller", "htc-vive-focus-3" },
	{ "/interaction_profiles/bytedance/pico_neo3_controller", "pico-neo3" },
	{ "/interaction_profiles/bytedance/pico4_controller", "pico-4" },
	{ "/interaction_profiles/bytedance/pico_g3_controller", "generic-trigger-squeeze-thumbstick" },
	{ "/interaction_profiles/khr/simple_controller", "generic-trigger-squeeze-thumbstick" },
};

static bool EndsWith(const string& s, const char* end)
{
	size_t n = strlen(end);
	return s.size() >= n && s.compare(s.size() - n, n, end) == 0;
}

static bool IsOwnControllerModel(const string& name)
{
	const std::vector<string>& names = OwnRenderModelNames();
	return std::binary_search(names.begin(), names.end(), name);
}

// The own controller model of the profile the hand's controller has now; empty without one
static string OwnModelOfHand(ITrackedDevice::TrackedDeviceType hand)
{
	auto sessionLock = xr_session.lock_shared(); // held while reading: a session restart waits for it
	BackendManager* backend = BackendManager::InstancePtr();
	std::shared_ptr<ITrackedDevice> dev = backend ? backend->GetDeviceByHand(hand) : nullptr;
	const InteractionProfile* profile = dev ? dev->GetInteractionProfile() : nullptr;
	if (!profile)
		return {};
	for (const auto& own : kOwnControllerModels)
		if (profile->GetPath() == own.path)
			return string(own.model) + (hand == ITrackedDevice::HAND_LEFT ? "-left" : "-right");
	return {};
}

// The own controller model a name is drawn with: one of their names; a hand's model (renderLeftHand, renderRightHand)
// as the controller in that hand; the Touch's and Index's names as those controllers (the Touch's as the one in that
// hand where it's a Touch Plus or Pro). Empty for any other name, or if the models aren't installed.
static string OwnModelName(const string& name)
{
	if (OwnRenderModelNames().empty())
		return {};
	if (IsOwnControllerModel(name))
		return name;
	bool left = name == "renderLeftHand" || name == "oculus_quest2_controller_left" || name == "{indexcontroller}valve_controller_knu_1_0_left";
	bool right = name == "renderRightHand" || name == "oculus_quest2_controller_right" || name == "{indexcontroller}valve_controller_knu_1_0_right";
	if (!left && !right)
		return {};
	string own = OwnModelOfHand(left ? ITrackedDevice::HAND_LEFT : ITrackedDevice::HAND_RIGHT);
	const char* hand = left ? "-left" : "-right";
	if (name.starts_with("oculus_quest2_controller_")) {
		if (own.empty() || !(own.starts_with("oculus-touch") || own.starts_with("meta-quest-touch")))
			own = string("oculus-touch-v3") + hand;
	} else if (name.starts_with("{indexcontroller}")) {
		own = string("valve-index") + hand;
	}
	return IsOwnControllerModel(own) ? own : string();
}

// Where an own model's files (in grip space) are in its device's space: the grip, as its profile places it. The
// profiles are made with the OpenXR instance: before there is one, the grip is taken as the device's pose.
static XrPosef OwnModelSpace(const string& own)
{
	XrPosef pose = { { 0, 0, 0, 1 }, { 0, 0, 0 } };
	if (!xr_ext)
		return pose;
	bool left = EndsWith(own, "-left");
	string model = own.substr(0, own.size() - (left ? 5 : 6));
	for (const auto& entry : kOwnControllerModels) {
		if (model != entry.model)
			continue;
		const InteractionProfile* profile = InteractionProfile::GetProfileByPath(entry.path);
		if (!profile)
			break;
		glm::mat4 gripInDevice = glm::affineInverse(profile->GetGripToSteamVRTransform(left ? ITrackedDevice::HAND_LEFT : ITrackedDevice::HAND_RIGHT));
		glm::quat rot = glm::normalize(glm::quat_cast(glm::mat3(gripInDevice)));
		pose.orientation = { rot.x, rot.y, rot.z, rot.w };
		pose.position = { gripInDevice[3].x, gripInDevice[3].y, gripInDevice[3].z };
		break;
	}
	return pose;
}

// A model by name: SteamVR's of that name where SteamVR has one, else OpenLamina's own controller model it's drawn with
static std::shared_ptr<const RmModel> FindModel(const string& name)
{
	if (std::shared_ptr<const RmModel> model = FindSteamVRRenderModel(name))
		return model;
	string own = OwnModelName(name);
	return own.empty() ? nullptr : FindOwnRenderModel(own, OwnModelSpace(own));
}

// The name a load is kept under: an own model's name (and component) for a name drawn with one, else the name
static string LoadName(const string& name)
{
	if (FindSteamVRRenderModel(name))
		return name;
	string own = OwnModelName(name);
	if (!own.empty())
		return own;
	size_t slash = name.rfind('/');
	if (slash != string::npos && slash > 0 && !FindSteamVRRenderModel(name.substr(0, slash))) {
		own = OwnModelName(name.substr(0, slash));
		if (!own.empty())
			return own + name.substr(slash);
	}
	return name;
}

// Copies value into an app's buffer, cut short and terminated if it doesn't fit; returns the size needed with the
// terminator, as SteamVR does, and BufferTooSmall in *error (when given) if the buffer can't hold both
static uint32_t CopyOutModelString(const string& value, char* buffer, uint32_t size, EVRRenderModelError* error = nullptr)
{
	if (error && size < value.size() + 1)
		*error = VRRenderModelError_BufferTooSmall;
	if (buffer && size) {
		size_t n = std::min<size_t>(value.size(), size - 1);
		memcpy(buffer, value.data(), n);
		buffer[n] = 0;
	}
	return (uint32_t)value.size() + 1;
}

// The models and textures handed out and not yet freed: Free* ignores anything else (a pointer freed twice, or one
// that never came from here) instead of corrupting the heap. Never freed: a Free* call can come during static
// destruction
static std::mutex& LiveLock()
{
	static std::mutex& lock = *new std::mutex;
	return lock;
}
static std::set<const void*>& LiveModels()
{
	static std::set<const void*>& models = *new std::set<const void*>;
	return models;
}
static std::set<const void*>& LiveTextures()
{
	static std::set<const void*>& textures = *new std::set<const void*>;
	return textures;
}

static void AddLive(std::set<const void*>& set, const void* p)
{
	std::lock_guard<std::mutex> guard(LiveLock());
	set.insert(p);
}

static bool TakeLive(std::set<const void*>& set, const void* p)
{
	std::lock_guard<std::mutex> guard(LiveLock());
	return set.erase(p) != 0;
}

#ifdef SUPPORT_DX11
// A D3D11 texture LoadTextureD3D11_Async handed out and FreeTextureD3D11 hasn't released
static std::set<const void*>& LiveD3D11Textures()
{
	static std::set<const void*>& textures = *new std::set<const void*>;
	return textures;
}
#endif

// SteamVR's textures, by the id their models give (from 1: OpenLamina's own models have none). A file that can't be
// read gets no id (its model has no texture), and is tried once. The decoded images are kept up to kMaxKeptTextureBytes,
// the least recently asked for let go first (a program that lists every model would otherwise hold them all); one let
// go is read from its file again when it's next asked for.
namespace {
constexpr size_t kMaxKeptTextureBytes = 128u * 1024 * 1024;
struct SteamVRTextures {
	std::mutex lock;
	// By a mesh's textures (several: their atlas); -1 couldn't be read
	std::map<std::vector<std::filesystem::path>, TextureID_t> ids;
	std::map<TextureID_t, std::vector<std::filesystem::path>> files;
	std::map<TextureID_t, std::shared_ptr<const Image>> kept;
	std::vector<TextureID_t> recent; // the kept ones, the most recently asked for last
	size_t keptBytes = 0;

	// Under lock: the image is the id's, asked for now
	void Keep(TextureID_t id, const std::shared_ptr<const Image>& image)
	{
		recent.erase(std::remove(recent.begin(), recent.end(), id), recent.end());
		recent.push_back(id);
		if (kept.emplace(id, image).second)
			keptBytes += image->rgba.size();
		// The one just asked for stays, whatever its size
		while (keptBytes > kMaxKeptTextureBytes && recent.size() > 1) {
			auto old = kept.find(recent.front());
			if (old != kept.end()) {
				keptBytes -= old->second->rgba.size();
				kept.erase(old);
			}
			recent.erase(recent.begin());
		}
	}
};
SteamVRTextures& Textures()
{
	static SteamVRTextures& t = *new SteamVRTextures;
	return t;
}
} // namespace

static TextureID_t SteamVRTextureId(const std::vector<std::filesystem::path>& file)
{
	if (file.empty())
		return -1;
	SteamVRTextures& t = Textures();
	{
		std::lock_guard<std::mutex> guard(t.lock);
		auto it = t.ids.find(file);
		if (it != t.ids.end())
			return it->second;
	}
	std::optional<Image> tex = LoadRenderModelTextures(file); // logged if it can't be read
	std::lock_guard<std::mutex> guard(t.lock);
	auto it = t.ids.find(file);
	if (it != t.ids.end())
		return it->second;
	TextureID_t id = tex ? (TextureID_t)t.files.size() + 1 : -1;
	t.ids.emplace(file, id);
	if (tex) {
		t.files.emplace(id, file);
		t.Keep(id, std::make_shared<const Image>(std::move(*tex)));
	}
	return id;
}

// The id of the texture of OpenLamina's own models: the hand colour, 1x1. SteamVR's ids are small numbers from 1
static constexpr TextureID_t kOwnTextureId = 1000000;

static std::shared_ptr<const Image> SteamVRTexture(TextureID_t id)
{
	SteamVRTextures& t = Textures();
	std::vector<std::filesystem::path> file;
	{
		std::lock_guard<std::mutex> guard(t.lock);
		auto it = t.kept.find(id);
		if (it != t.kept.end()) {
			std::shared_ptr<const Image> image = it->second;
			t.Keep(id, image);
			return image;
		}
		auto known = t.files.find(id);
		if (known == t.files.end())
			return nullptr;
		file = known->second;
	}
	// Let go since it was read: read again (gone or damaged meanwhile: no texture)
	std::optional<Image> tex = LoadRenderModelTextures(file);
	if (!tex)
		return nullptr;
	auto image = std::make_shared<const Image>(std::move(*tex));
	std::lock_guard<std::mutex> guard(t.lock);
	t.Keep(id, image);
	return image;
}

// A colour channel (0 to 1) as a byte: a value outside is the nearest end, one that is not a number 0
static uint8_t ChannelByte(float v)
{
	if (!(v > 0))
		return 0;
	if (v >= 1)
		return 255;
	return (uint8_t)(v * 255);
}

static std::shared_ptr<const Image> OwnTexture()
{
	vr::HmdColor_t colour = oovr_global_configuration.HandColour();
	auto image = std::make_shared<Image>();
	image->width = 1;
	image->height = 1;
	image->rgba = { ChannelByte(colour.r), ChannelByte(colour.g), ChannelByte(colour.b), ChannelByte(colour.a) };
	return image;
}

// The pixels of a texture id: SteamVR's, or the hand colour for the own models'; null for any other id
static std::shared_ptr<const Image> ResolveTexture(TextureID_t id)
{
	if (id == kOwnTextureId)
		return OwnTexture();
	return SteamVRTexture(id);
}

// A model of SteamVR's install by name: a model ("<name>" or "{<driver>}<name>"), one of its components
// ("<model>/<component>", what GetComponentRenderModelName answers) or an OBJ file's absolute path (what
// GetRenderModelOriginalPath answers; that one loads without SteamVR). nullopt for anything it can't load.
static std::optional<RmMesh> SteamVRMesh(const string& name)
{
	if (std::shared_ptr<const RmModel> model = FindSteamVRRenderModel(name))
		return LoadRenderModelMesh(*model, "");
	size_t slash = name.rfind('/');
	if (slash != string::npos && slash > 0) {
		if (std::shared_ptr<const RmModel> model = FindSteamVRRenderModel(name.substr(0, slash)))
			return LoadRenderModelMesh(*model, name.substr(slash + 1));
	}
	try {
		std::filesystem::path file = PathFromUtf8(name);
		std::error_code ec;
		if (file.is_absolute() && file.extension() == ".obj" && std::filesystem::is_regular_file(file, ec))
			return LoadObjMesh(file);
	} catch (const std::exception&) {
	}
	return std::nullopt;
}

static const RmComponent* FindComponent(const RmModel& model, const char* name)
{
	if (!name)
		return nullptr;
	for (const RmComponent& c : model.components)
		if (c.name == name)
			return &c;
	return nullptr;
}

// The OpenVR names of every model: SteamVR's, then OpenLamina's own that SteamVR doesn't have, then its controllers'
static const std::vector<string>& AllModelNames()
{
	static const std::vector<string>& names = *[] {
		auto* all = new std::vector<string>(SteamVRRenderModelNames());
		for (const char* own : kOwnModelNames)
			if (std::find(all->begin(), all->end(), own) == all->end())
				all->push_back(own);
		for (const string& own : OwnRenderModelNames())
			all->push_back(own);
		return all;
	}();
	return names;
}

// The model built into the DLL as the game gets it, built once
struct BuiltInMesh {
	std::vector<RmVertex> vertices;
	std::vector<uint16_t> indices;
};

static std::shared_ptr<const BuiltInMesh> BuildBuiltInMesh(int rid, float sided)
{
	std::vector<char> resource = ReadBuiltInResource(rid, RES_T_OBJ);
	if (resource.empty())
		return nullptr;
	auto mesh = std::make_shared<BuiltInMesh>();
	// The models are OpenLamina's own, so one that can't be read is a build problem: it has no model
	if (!ParseObj(std::string_view(resource.data(), resource.size()), mesh->vertices, mesh->indices, nullptr)) {
		OOVR_LOGF("Built-in render model %d can't be read", rid);
		return nullptr;
	}

	// Transform to line up the model with the Touch controller
	mat4 modelTransform = mat4(glm::rotate(sided * std::numbers::pi_v<float> / 2, vec3(0, 0, 1)));
	modelTransform[3] = vec4(sided * 0.015f, 0.0f, 0.03f, 1.0f);

	// SteamVR rotates it's models 180deg around the Y axis for some reason
	modelTransform *= mat4(glm::rotate(std::numbers::pi_v<float>, vec3(0, 1, 0)));

	mat4 transform = glm::inverse(rid == RES_O_VIVE_TRACKER ? BaseCompositor::GetTrackerTransform() : BaseCompositor::GetHandTransform()) * modelTransform;

	for (RmVertex& out : mesh->vertices) {
		// Maya exports in cm, so translate that to meters
		vec4 vertex = transform * vec4(out.vPosition.v[0] * 0.01f, out.vPosition.v[1] * 0.01f, out.vPosition.v[2] * 0.01f, 1.0f);
		out.vPosition.v[0] = vertex.x;
		out.vPosition.v[1] = vertex.y;
		out.vPosition.v[2] = vertex.z;
	}
	return mesh;
}

// A model for the game from the built, as a copy it frees with FreeRenderModel
static RenderModel_t* MakeRenderModel(const std::vector<RmVertex>& vertices, const std::vector<uint16_t>& indices, TextureID_t texture)
{
	auto* rm = new RenderModel_t();
	rm->unVertexCount = (uint32_t)vertices.size();
	auto* vertexData = new OOVR_RenderModel_Vertex_t[vertices.size()];
	memcpy(vertexData, vertices.data(), vertices.size() * sizeof(OOVR_RenderModel_Vertex_t));
	rm->rVertexData = vertexData;
	auto* indexData = new uint16_t[indices.size()];
	memcpy(indexData, indices.data(), indices.size() * sizeof(uint16_t));
	rm->rIndexData = indexData;
	rm->unTriangleCount = (uint32_t)(indices.size() / 3);
	rm->diffuseTextureId = texture;
	AddLive(LiveModels(), rm);
	return rm;
}

// A model as loaded, kept for the process: SteamVR's (with its texture's id) or the built-in one
namespace {
struct LoadedModel {
	EVRRenderModelError error = VRRenderModelError_None; // None, NotSupported, or InvalidModel for a name with no model
	std::vector<RmVertex> vertices;
	std::vector<uint16_t> indices;
	TextureID_t texture = -1;
};

// The models being loaded and loaded, by name. A load runs on a thread of its own; LoadRenderModel_Async answers
// Loading until it's done. A name's answer, a failure too, is kept for the process. Never freed: a load can still be
// running when the program exits.
struct ModelLoads {
	// More names than SteamVR has models and components: past it, a name isn't loaded
	static constexpr size_t kMaxNames = 4096;
	std::mutex lock;
	std::map<string, std::shared_future<std::shared_ptr<const LoadedModel>>> byName;
};
ModelLoads& Loads()
{
	static ModelLoads& loads = *new ModelLoads;
	return loads;
}
} // namespace

static std::shared_ptr<const LoadedModel> LoadModel(const string& name, std::shared_ptr<const RmModel> own);

EVRRenderModelError BaseRenderModels::LoadRenderModel_Async(const char* pchRenderModelName, RenderModel_t** renderModel)
{
	if (!pchRenderModelName || !renderModel)
		return VRRenderModelError_InvalidArg;
	*renderModel = nullptr;
	string name = LoadName(pchRenderModelName);

	// Started on the first call for a name, answered once it's done, at once from then on
	std::shared_future<std::shared_ptr<const LoadedModel>> load;
	{
		ModelLoads& loads = Loads();
		std::lock_guard<std::mutex> guard(loads.lock);
		auto it = loads.byName.find(name);
		if (it == loads.byName.end()) {
			if (loads.byName.size() >= ModelLoads::kMaxNames)
				OC_UNSUPPORTED(VRRenderModelError_NotSupported, "LoadRenderModel_Async: too many model names, '%s' isn't loaded", pchRenderModelName);
			OOVR_LOGF("LoadRenderModel_Async %s", pchRenderModelName);
			// An own controller model is found here, where the profiles are; its load's thread only reads it
			std::shared_ptr<const RmModel> own;
			if (string base = name.substr(0, name.find('/')); IsOwnControllerModel(base) && !FindSteamVRRenderModel(base))
				own = FindModel(base);
			std::shared_future<std::shared_ptr<const LoadedModel>> started;
			try {
				started = std::async(std::launch::async, LoadModel, name, own).share();
			} catch (const std::exception& e) {
				OC_UNSUPPORTED(VRRenderModelError_NotSupported, "LoadRenderModel_Async: can't start loading '%s': %s", pchRenderModelName, e.what());
			}
			loads.byName.emplace(name, std::move(started));
			return VRRenderModelError_Loading;
		}
		load = it->second;
	}
	if (load.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		return VRRenderModelError_Loading;

	std::shared_ptr<const LoadedModel> loaded = load.get();
	if (!loaded)
		return VRRenderModelError_InvalidModel;
	if (loaded->error == VRRenderModelError_InvalidModel) {
		// A model OpenLamina doesn't have: games then draw none, or their own
		OC_UNSUPPORTED(VRRenderModelError_InvalidModel, "LoadRenderModel_Async: no render model '%s'", pchRenderModelName);
	}
	if (loaded->error != VRRenderModelError_None)
		return loaded->error;
	*renderModel = MakeRenderModel(loaded->vertices, loaded->indices, loaded->texture);
	return VRRenderModelError_None;
}

EVRRenderModelError BaseRenderModels::LoadRenderModelNow(const char* pchRenderModelName, RenderModel_t** renderModel)
{
	EVRRenderModelError err;
	while ((err = LoadRenderModel_Async(pchRenderModelName, renderModel)) == VRRenderModelError_Loading)
		WaitForLoads();
	return err;
}

void BaseRenderModels::WaitForLoads()
{
	std::vector<std::shared_future<std::shared_ptr<const LoadedModel>>> pending;
	{
		ModelLoads& loads = Loads();
		std::lock_guard<std::mutex> guard(loads.lock);
		for (auto& [name, load] : loads.byName)
			pending.push_back(load);
	}
	for (auto& load : pending)
		load.wait();
}

BaseRenderModels::~BaseRenderModels()
{
	// A load mustn't outlive the runtime it's running in
	WaitForLoads();
}

// The model of a name, on a load's thread: own's where it's given (name being its name or "<name>/<component>"), else
// SteamVR's of that name as SteamVR answers it, else (no SteamVR, or a model it can't load) OpenLamina's built-in one
static std::shared_ptr<const LoadedModel> BuildModel(const string& name, const std::shared_ptr<const RmModel>& own)
{
	auto loaded = std::make_shared<LoadedModel>();
	size_t slash = name.find('/');
	string component = slash == string::npos ? "" : name.substr(slash + 1);
	if (std::optional<RmMesh> found = own ? LoadRenderModelMesh(*own, component) : SteamVRMesh(name)) {
		loaded->vertices = std::move(found->vertices);
		loaded->indices = std::move(found->indices);
		loaded->texture = SteamVRTextureId(found->textures);
		return loaded;
	}

	int rid;
	float sided;

	// todo: start loading correct models, !359 related
	if (name == "renderLeftHand") {
		rid = RES_O_HAND_LEFT;
		sided = 1;
	} else if (name == "renderRightHand") {
		rid = RES_O_HAND_RIGHT;
		sided = -1;
	} else if (name == "oculus_quest2_controller_left") {
		rid = RES_O_HAND_LEFT;
		sided = 1;
	} else if (name == "oculus_quest2_controller_right") {
		rid = RES_O_HAND_RIGHT;
		sided = -1;
	} else if (name == "{indexcontroller}valve_controller_knu_1_0_left") {
		rid = RES_O_HAND_LEFT;
		sided = 1;
	} else if (name == "{indexcontroller}valve_controller_knu_1_0_right") {
		rid = RES_O_HAND_RIGHT;
		sided = -1;
	} else if (name == "vive_tracker") {
		rid = RES_O_VIVE_TRACKER;
		sided = 1;
	} else if (name == "oculusHmdRenderModel") {
		// no model for the HMD
		loaded->error = VRRenderModelError_NotSupported;
		return loaded;
	} else {
		// A model OpenLamina doesn't have: games then draw none, or their own
		loaded->error = VRRenderModelError_InvalidModel;
		return loaded;
	}

	std::shared_ptr<const BuiltInMesh> mesh = BuildBuiltInMesh(rid, sided);
	if (!mesh) {
		loaded->error = VRRenderModelError_NotSupported;
		return loaded;
	}
	loaded->vertices = mesh->vertices;
	loaded->indices = mesh->indices;
	loaded->texture = kOwnTextureId;
	return loaded;
}

// BuildModel, with a model it can't build (out of memory) as one it doesn't support
static std::shared_ptr<const LoadedModel> LoadModel(const string& name, std::shared_ptr<const RmModel> own)
{
	try {
		return BuildModel(name, own);
	} catch (const std::exception& e) {
		OOVR_LOGF("Render model %s can't be loaded: %s", name.c_str(), e.what());
		auto failed = std::make_shared<LoadedModel>();
		failed->error = VRRenderModelError_NotSupported;
		return failed;
	}
}

void BaseRenderModels::FreeRenderModel(RenderModel_t* renderModel)
{
	if (!renderModel || !TakeLive(LiveModels(), renderModel)) {
		OOVR_LOG_ONCE("FreeRenderModel: not a render model from LoadRenderModel_Async (or freed already), ignored");
		return;
	}
	delete[] renderModel->rVertexData;
	delete[] renderModel->rIndexData;
	delete renderModel;
}

namespace {
// What an inline render model's ulInternalHandle points at
struct InlineRenderModel {
	RenderModel_t* model;
	RenderModel_TextureMap_t* texture;
};
std::mutex inlineModelsLock;
std::set<InlineRenderModel*> inlineModels;
} // namespace

static_assert(sizeof(OOVR_RenderModel_Vertex_t) == sizeof(vr::IVRRenderModels_001::RenderModel_Vertex_t),
    "the old vertex is today's");
// The old inline model as the SDK lays it out (packed to 4 on Linux, 8 on Windows)
#if defined(__linux__) || defined(__APPLE__)
static_assert(offsetof(vr::IVRRenderModels_001::RenderModel_t, rIndexData) == (sizeof(void*) == 8 ? 20 : 16), "Linux packs to 4");
#else
static_assert(offsetof(vr::IVRRenderModels_001::RenderModel_t, rIndexData) == (sizeof(void*) == 8 ? 24 : 16), "Windows packs to 8");
#endif

bool BaseRenderModels::LoadRenderModelInline(const char* pchRenderModelName, vr::IVRRenderModels_001::RenderModel_t* pRenderModel)
{
	if (!pRenderModel)
		return false;
	*pRenderModel = {};

	// One call answers the whole model
	RenderModel_t* model = nullptr;
	if (LoadRenderModelNow(pchRenderModelName, &model) != VRRenderModelError_None || !model)
		return false;
	RenderModel_TextureMap_t* texture = nullptr;
	if (LoadTexture_Async(model->diffuseTextureId >= 0 ? model->diffuseTextureId : kOwnTextureId, &texture) != VRRenderModelError_None || !texture) {
		FreeRenderModel(model);
		return false;
	}

	auto* holder = new InlineRenderModel{ model, texture };
	{
		std::lock_guard<std::mutex> guard(inlineModelsLock);
		inlineModels.insert(holder);
	}
	pRenderModel->ulInternalHandle = (uint64_t)(uintptr_t)holder;
	pRenderModel->rVertexData = (const vr::IVRRenderModels_001::RenderModel_Vertex_t*)model->rVertexData;
	pRenderModel->unVertexCount = model->unVertexCount;
	pRenderModel->rIndexData = model->rIndexData;
	pRenderModel->unTriangleCount = model->unTriangleCount;
	pRenderModel->diffuseTexture.unWidth = texture->unWidth;
	pRenderModel->diffuseTexture.unHeight = texture->unHeight;
	pRenderModel->diffuseTexture.rubTextureMapData = texture->rubTextureMapData;
	return true;
}

void BaseRenderModels::FreeRenderModelInline(vr::IVRRenderModels_001::RenderModel_t* pRenderModel)
{
	if (!pRenderModel)
		return;
	auto* holder = (InlineRenderModel*)(uintptr_t)pRenderModel->ulInternalHandle;
	{
		std::lock_guard<std::mutex> guard(inlineModelsLock);
		if (!inlineModels.erase(holder)) {
			OOVR_LOG_ONCE("FreeRenderModel: not a render model from LoadRenderModel (or freed already), ignored");
			return;
		}
	}
	FreeTexture(holder->texture);
	FreeRenderModel(holder->model);
	delete holder;
	*pRenderModel = {};
}

EVRRenderModelError BaseRenderModels::LoadTexture_Async(TextureID_t textureId, RenderModel_TextureMap_t** texture)
{
	if (!texture)
		return VRRenderModelError_InvalidArg;
	*texture = nullptr;

	// A SteamVR model's texture, or the own models' hand colour
	std::shared_ptr<const Image> image = ResolveTexture(textureId);
	if (!image)
		return VRRenderModelError_InvalidArg;

	// The newest layout of the struct: programs built with an older SDK read a prefix of it
	auto* tx = new RenderModel_TextureMap_t();
	tx->unWidth = (uint16_t)image->width;
	tx->unHeight = (uint16_t)image->height;
	uint8_t* pixels = new uint8_t[image->rgba.size()];
	memcpy(pixels, image->rgba.data(), image->rgba.size());
	tx->rubTextureMapData = pixels;
	tx->format = 0;
	tx->unMipLevels = 1;
	AddLive(LiveTextures(), tx);
	*texture = tx;
	return VRRenderModelError_None;
}

void BaseRenderModels::FreeTexture(RenderModel_TextureMap_t* texture)
{
	if (!texture || !TakeLive(LiveTextures(), texture)) {
		OOVR_LOG_ONCE("FreeTexture: not a texture from LoadTexture_Async (or freed already), ignored");
		return;
	}
	delete[] texture->rubTextureMapData;
	delete texture;
}

EVRRenderModelError BaseRenderModels::LoadTextureD3D11_Async(TextureID_t textureId, void* pD3D11Device, void** ppD3D11Texture2D)
{
	if (ppD3D11Texture2D)
		*ppD3D11Texture2D = nullptr;
	std::shared_ptr<const Image> tex = ResolveTexture(textureId);
	if (!tex)
		return VRRenderModelError_InvalidArg;
#ifndef SUPPORT_DX11
	OC_UNSUPPORTED(VRRenderModelError_NotSupported, "LoadTextureD3D11_Async: this build has no D3D11 support");
#else
	// Windows never maps the first 64 KiB, so a "device" there is a bad argument, not a device
	if (!ppD3D11Texture2D || (uintptr_t)pD3D11Device < 0x10000)
		return VRRenderModelError_InvalidArg;

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = tex->width;
	desc.Height = tex->height;
	// The whole mip chain, each level with its own pixels
	std::vector<Image> mips = BuildMipChain(*tex, 0);
	desc.MipLevels = (UINT)mips.size();
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	std::vector<D3D11_SUBRESOURCE_DATA> init(mips.size());
	for (size_t i = 0; i < mips.size(); i++) {
		init[i].pSysMem = mips[i].rgba.data();
		init[i].SysMemPitch = mips[i].width * 4;
		init[i].SysMemSlicePitch = 0;
	}
	ID3D11Texture2D* out = nullptr;
	HRESULT hr = ((ID3D11Device*)pD3D11Device)->CreateTexture2D(&desc, init.data(), &out);
	if (FAILED(hr) || !out)
		OC_UNSUPPORTED(VRRenderModelError_InvalidTexture, "LoadTextureD3D11_Async: CreateTexture2D failed (0x%08lx)", (unsigned long)hr);
	AddLive(LiveD3D11Textures(), out);
	*ppD3D11Texture2D = out;
	return VRRenderModelError_None;
#endif
}

EVRRenderModelError BaseRenderModels::LoadIntoTextureD3D11_Async(TextureID_t textureId, void* pDstTexture)
{
#ifndef SUPPORT_DX11
	OC_UNSUPPORTED(VRRenderModelError_NotSupported, "LoadIntoTextureD3D11_Async: this build has no D3D11 support");
#else
	ID3D11Texture2D* output = (ID3D11Texture2D*)pDstTexture;
	// Windows never maps the first 64 KiB, so a "texture" there is a bad argument, not a texture
	if ((uintptr_t)output < 0x10000)
		return VRRenderModelError_InvalidArg;

	std::shared_ptr<const Image> source = ResolveTexture(textureId);
	if (!source)
		return VRRenderModelError_InvalidArg;

	// D3D11 setup
	ID3D11Device* device;
	ID3D11DeviceContext* context;
	output->GetDevice(&device);
	device->GetImmediateContext(&context);

	D3D11_TEXTURE2D_DESC desc;
	output->GetDesc(&desc);

	// Only four-byte RGBA targets can take the pixels
	if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
	    && desc.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS) {
		context->Release();
		device->Release();
		return VRRenderModelError_InvalidTexture;
	}

	// Since we don't know what the map flags are, make an identical texture and copy across: the texture scaled to the
	// target's size, with a mip chain down from it for every array slice
	Image base = ResampleImage(*source, desc.Width, desc.Height);
	std::vector<Image> mips = BuildMipChain(base, desc.MipLevels);
	std::vector<D3D11_SUBRESOURCE_DATA> init;
	for (UINT slice = 0; slice < desc.ArraySize; slice++) {
		for (UINT level = 0; level < desc.MipLevels; level++) {
			const Image& mip = mips[level < mips.size() ? level : mips.size() - 1];
			D3D11_SUBRESOURCE_DATA data{};
			data.pSysMem = mip.rgba.data();
			data.SysMemPitch = mip.width * 4;
			init.push_back(data);
		}
	}

	ID3D11Texture2D* tempTex = nullptr;
	HRESULT hr = device->CreateTexture2D(&desc, init.data(), &tempTex);
	if (FAILED(hr)) {
		// The app's texture keeps what it had
		context->Release();
		device->Release();
		OC_UNSUPPORTED(VRRenderModelError_InvalidTexture, "LoadIntoTextureD3D11_Async: CreateTexture2D failed (0x%08lx)", (unsigned long)hr);
	}

	// Copy over the texture
	context->CopyResource(output, tempTex);

	// Cleanup
	tempTex->Release();
	context->Release();
	device->Release();

	return VRRenderModelError_None;
#endif
}

void BaseRenderModels::FreeTextureD3D11(void* pD3D11Texture2D)
{
#ifdef SUPPORT_DX11
	// Only what LoadTextureD3D11_Async handed out, once
	if (pD3D11Texture2D && TakeLive(LiveD3D11Textures(), pD3D11Texture2D))
		((ID3D11Texture2D*)pD3D11Texture2D)->Release();
#endif
}

uint32_t BaseRenderModels::GetRenderModelName(uint32_t unRenderModelIndex, VR_OUT_STRING() char* pchRenderModelName, uint32_t unRenderModelNameLen)
{
	// SteamVR's models if it is installed, then OpenLamina's own; the size with the terminator, as SteamVR answers
	const std::vector<string>& names = AllModelNames();
	if (unRenderModelIndex >= names.size()) {
		if (pchRenderModelName && unRenderModelNameLen)
			pchRenderModelName[0] = 0;
		return 0;
	}
	return CopyOutModelString(names[unRenderModelIndex], pchRenderModelName, unRenderModelNameLen);
}

uint32_t BaseRenderModels::GetRenderModelCount()
{
	// OpenLamina's own three, after SteamVR's models
	return (uint32_t)AllModelNames().size();
}

uint32_t BaseRenderModels::GetComponentCount(const char* pchRenderModelName)
{
	if (pchRenderModelName) {
		if (std::shared_ptr<const RmModel> model = FindModel(pchRenderModelName))
			return (uint32_t)model->components.size();
	}

	// OpenLamina's own models have one static component, named after the model
	return pchRenderModelName && IsKnownRenderModel(pchRenderModelName) ? 1 : 0;
}

uint32_t BaseRenderModels::GetComponentName(const char* pchRenderModelName, uint32_t unComponentIndex,
    char* pchComponentName, uint32_t unComponentNameLen)
{
	if (pchRenderModelName) {
		if (std::shared_ptr<const RmModel> model = FindModel(pchRenderModelName)) {
			if (unComponentIndex >= model->components.size()) {
				if (pchComponentName && unComponentNameLen)
					pchComponentName[0] = 0;
				return 0;
			}
			return CopyOutModelString(model->components[unComponentIndex].name, pchComponentName, unComponentNameLen);
		}
	}

	if (!pchRenderModelName || !IsKnownRenderModel(pchRenderModelName)) {
		if (pchComponentName && unComponentNameLen)
			pchComponentName[0] = 0;
		return 0;
	}

	// Only the first component exists
	if (unComponentIndex != 0) {
		if (pchComponentName && unComponentNameLen)
			pchComponentName[0] = 0;
		return 0;
	}

	// The component is named after its model; cut short if the buffer is too small, with the size needed returned
	return CopyOutModelString(pchRenderModelName, pchComponentName, unComponentNameLen);
}

uint64_t BaseRenderModels::GetComponentButtonMask(const char* pchRenderModelName, const char* pchComponentName)
{
	// A SteamVR model's component: the button and the axis its motion follows
	std::shared_ptr<const RmModel> model = pchRenderModelName ? FindModel(pchRenderModelName) : nullptr;
	const RmComponent* c = model ? FindComponent(*model, pchComponentName) : nullptr;
	if (!c || !c->motion.isObject())
		return 0;
	uint64_t mask = 0;
	const Json::Value& button = c->motion["controller_button"];
	if (button.isIntegral() && button.asInt64() >= 0 && button.asInt64() < 64)
		mask |= vr::ButtonMaskFromId((vr::EVRButtonId)button.asInt());
	const Json::Value& axis = c->motion["controller_axis"];
	if (axis.isIntegral() && axis.asInt64() >= 0 && axis.asInt64() < vr::k_unControllerStateAxisCount)
		mask |= vr::ButtonMaskFromId((vr::EVRButtonId)(vr::k_EButton_Axis0 + axis.asInt()));
	return mask;
}

uint32_t BaseRenderModels::GetComponentRenderModelName(const char* pchRenderModelName, const char* pchComponentName,
    char* componentModelName, uint32_t componentModelNameLen)
{
	// A SteamVR model's component with something to draw: "<model>/<component>", which LoadRenderModel_Async loads
	if (pchRenderModelName) {
		if (std::shared_ptr<const RmModel> model = FindModel(pchRenderModelName)) {
			const RmComponent* c = FindComponent(*model, pchComponentName);
			if (!c || c->objFile.empty()) {
				if (componentModelName && componentModelNameLen)
					componentModelName[0] = 0;
				return 0;
			}
			return CopyOutModelString(model->name + "/" + c->name, componentModelName, componentModelNameLen);
		}
	}

	if (!pchRenderModelName || !pchComponentName || !IsKnownRenderModel(pchRenderModelName)
	    || strcmp(pchRenderModelName, pchComponentName) != 0) {
		// Only the model's one component (named after it) has a render model
		if (componentModelName && componentModelNameLen)
			componentModelName[0] = 0;
		return 0;
	}

	return CopyOutModelString(pchRenderModelName, componentModelName, componentModelNameLen);
}

bool BaseRenderModels::GetComponentStateForDevicePath(const char* pchRenderModelName, const char* pchComponentName,
    vr::VRInputValueHandle_t devicePath, const OOVR_RenderModel_ControllerMode_State_t* pState,
    OOVR_RenderModel_ComponentState_t* pComponentState)
{
	// The controller state of the device the path names, so the model's buttons and sticks move with it
	vr::VRControllerState_t state{};
	const vr::VRControllerState_t* controllerState = nullptr;
	if (BaseInput* input = GetUnsafeBaseInput()) {
		BaseInput::InputOriginInfo_t info{};
		if (input->GetOriginTrackedDeviceInfo(devicePath, &info, sizeof(info)) == vr::VRInputError_None
		    && input->GetLegacyControllerState(info.trackedDeviceIndex, &state))
			controllerState = &state;
	}
	return GetComponentState(pchRenderModelName, pchComponentName, controllerState, pState, pComponentState);
}

bool BaseRenderModels::GetComponentState(const char* pchRenderModelName, const char* pchComponentName,
    const vr::VRControllerState_t* pControllerState, const OOVR_RenderModel_ControllerMode_State_t* pState,
    OOVR_RenderModel_ComponentState_t* pComponentState)
{
	// On the Quest 2 Touch controllers, here's SteamVR's poses for the base, handgrip and tip components:
	// Left  base:     mat4x4((-1.000000, 0.000000, 0.000000, 0.000000), (0.000000, 0.999976, 0.006981, 0.000000), (-0.000000, 0.006981, -0.999976, 0.000000), (-0.003400, -0.003400, 0.149100, 1.000000))
	// Right base:     mat4x4((-1.000000, 0.000000, 0.000000, 0.000000), (0.000000, 0.999976, 0.006981, 0.000000), (-0.000000, 0.006981, -0.999976, 0.000000), (0.003400, -0.003400, 0.149100, 1.000000))
	// Left  handgrip: mat4x4((1.000000, 0.000000, 0.000000, 0.000000), (0.000000, 0.996138, 0.087799, 0.000000), (0.000000, -0.087799, 0.996138, 0.000000), (0.000000, 0.003000, 0.097000, 1.000000))
	// Right handgrip: mat4x4((1.000000, 0.000000, 0.000000, 0.000000), (0.000000, 0.996138, 0.087799, 0.000000), (0.000000, -0.087799, 0.996138, 0.000000), (0.000000, 0.003000, 0.097000, 1.000000))
	// Left  tip:      mat4x4((1.000000, 0.000000, 0.000000, 0.000000), (0.000000, 0.794415, -0.607376, 0.000000), (0.000000, 0.607376, 0.794415, 0.000000), (0.016694, -0.025220, 0.024687, 1.000000))
	// Right tip:      mat4x4((1.000000, 0.000000, 0.000000, 0.000000), (0.000000, 0.794415, -0.607376, 0.000000), (0.000000, 0.607376, 0.794415, 0.000000), (-0.016694, -0.025220, 0.024687, 1.000000))

	if (!pComponentState)
		return false;
	ZeroMemory(pComponentState, sizeof(*pComponentState));
	if (!pchComponentName)
		return false;

	std::string componentName = pchComponentName;

	// A SteamVR or own controller model's component: where its motion puts it for the controller state (at rest without one)
	const string own = pchRenderModelName ? OwnModelName(pchRenderModelName) : string();
	std::shared_ptr<const RmModel> steamvr = pchRenderModelName ? FindSteamVRRenderModel(pchRenderModelName) : nullptr;
	if (!steamvr && !own.empty())
		steamvr = FindOwnRenderModel(own, OwnModelSpace(own));
	if (const RmComponent* c = steamvr ? FindComponent(*steamvr, pchComponentName) : nullptr) {
		float value = 0;
		vr::HmdVector2_t axis = { { 0, 0 } };
		bool pressed = false, touched = false;
		const Json::Value& m = c->motion;
		if (pControllerState && m.isObject()) {
			const Json::Value& axisIndex = m["controller_axis"];
			if (axisIndex.isIntegral() && axisIndex.asInt64() >= 0 && axisIndex.asInt64() < vr::k_unControllerStateAxisCount) {
				int n = axisIndex.asInt();
				const vr::VRControllerAxis_t& a = pControllerState->rAxis[n];
				axis = { { a.x, a.y } };
				uint64_t bit = vr::ButtonMaskFromId((vr::EVRButtonId)(vr::k_EButton_Axis0 + n));
				pressed = (pControllerState->ulButtonPressed & bit) != 0;
				touched = (pControllerState->ulButtonTouched & bit) != 0;
				const Json::Value& part = m["controller_axis_component"];
				value = part.isIntegral() && part.asInt64() == 1 ? a.y : a.x;
			}
			const Json::Value& button = m["controller_button"];
			if (button.isIntegral() && button.asInt64() >= 0 && button.asInt64() < 64) {
				uint64_t bit = vr::ButtonMaskFromId((vr::EVRButtonId)button.asInt());
				pressed = pressed || (pControllerState->ulButtonPressed & bit) != 0;
				touched = touched || (pControllerState->ulButtonTouched & bit) != 0;
				if (!axisIndex.isIntegral())
					value = pressed ? 1.0f : 0.0f;
			}
			// A pad or stick moves with its axis; how far it's pressed is its click
			if (c->motionType == "trackpad" || c->motionType == "joystick")
				value = pressed ? 1.0f : 0.0f;
		}
		glm::mat4 motion = X2G_om34_pose(ModelComponentPose(*steamvr, *c, value, axis));
		pComponentState->mTrackingToComponentRenderModel = G2S_m34(motion);
		pComponentState->mTrackingToComponentLocal = G2S_m34(motion * X2G_om34_pose(ModelComponentLocal(*steamvr, *c)));
		uint32_t properties = 0;
		if (c->motionType.empty())
			properties |= VRComponentProperty_IsStatic;
		if (ComponentVisible(*c, touched, pressed))
			properties |= VRComponentProperty_IsVisible;
		if (touched)
			properties |= VRComponentProperty_IsTouched;
		if (pressed)
			properties |= VRComponentProperty_IsPressed;
		pComponentState->uProperties = properties;
		return true;
	}

	// A known model's one component, named after the model: where the model is, shown and still
	if (pchRenderModelName && IsKnownRenderModel(pchRenderModelName) && strcmp(pchRenderModelName, pchComponentName) == 0) {
		const glm::mat4 identity = glm::identity<glm::mat4>();
		pComponentState->mTrackingToComponentRenderModel = G2S_m34(identity);
		pComponentState->mTrackingToComponentLocal = G2S_m34(identity);
		pComponentState->uProperties = VRComponentProperty_IsVisible | VRComponentProperty_IsStatic;
		return true;
	}

	ITrackedDevice::TrackedDeviceType hand = ITrackedDevice::HAND_NONE;
	if (pchRenderModelName) {
		std::string renderModelName = pchRenderModelName;
		if (renderModelName == "renderLeftHand") {
			hand = ITrackedDevice::HAND_LEFT;
		} else if (renderModelName == "renderRightHand") {
			hand = ITrackedDevice::HAND_RIGHT;
		} else if (renderModelName == "vive_tracker") {
			hand = ITrackedDevice::GENERIC_TRACKER;
		} else if (IsOwnRenderModel(renderModelName.c_str()) && renderModelName.ends_with("_left")) {
			hand = ITrackedDevice::HAND_LEFT;
		} else if (IsOwnRenderModel(renderModelName.c_str()) && renderModelName.ends_with("_right")) {
			hand = ITrackedDevice::HAND_RIGHT;
		} else if (!own.empty()) {
			// An own controller model's tip, base and handgrip: the profile's, as for the hand's own model
			hand = EndsWith(own, "-left") ? ITrackedDevice::HAND_LEFT : ITrackedDevice::HAND_RIGHT;
		}
	}

	// See if we can get it properly
	bool success = TryGetComponentState(hand, componentName, pComponentState);
	if (success)
		return true;

	// A built-in model's tip and handgrip always exist: where the model is, until its device can tell more
	if (pchRenderModelName && (IsOwnRenderModel(pchRenderModelName) || !own.empty())
	    && (componentName == "tip" || componentName == "handgrip")) {
		const glm::mat4 identity = glm::identity<glm::mat4>();
		pComponentState->mTrackingToComponentRenderModel = G2S_m34(identity);
		pComponentState->mTrackingToComponentLocal = G2S_m34(identity);
		pComponentState->uProperties = VRComponentProperty_IsVisible | VRComponentProperty_IsStatic;
		return true;
	}

	OOVR_LOG_ONCE("GetComponentState: unknown component");
	ZeroMemory(pComponentState, sizeof(*pComponentState));
	return false;
}

bool BaseRenderModels::TryGetComponentState(ITrackedDevice::TrackedDeviceType hand, const std::string& componentName, OOVR_RenderModel_ComponentState_t* result)
{
	auto sessionLock = xr_session.lock_shared(); // held while reading: a session restart waits for it
	// Only the hands have a device to ask (a tracker isn't found by hand)
	if ((hand != ITrackedDevice::HAND_LEFT && hand != ITrackedDevice::HAND_RIGHT) || !BackendManager::InstancePtr())
		return false;

	std::shared_ptr<ITrackedDevice> dev = BackendManager::Instance().GetDeviceByHand(hand);
	if (!dev)
		return false;

	// See if there's a manually-defined transform
	const InteractionProfile* profile = dev->GetInteractionProfile();
	if (profile) {
		std::optional<glm::mat4> transform = profile->GetComponentTransform(hand, componentName);
		if (transform) {
			result->mTrackingToComponentLocal = G2S_m34(transform.value());
			result->mTrackingToComponentRenderModel = G2S_m34(transform.value());
			result->uProperties = VRComponentProperty_IsVisible | VRComponentProperty_IsStatic;
			return true;
		}
	}

	// If the hand position is offset, then this needs to account for it
	glm::mat4 handToGripSpace = glm::identity<glm::mat4>();
	if (profile) {
		handToGripSpace = glm::affineInverse(profile->GetGripToSteamVRTransform(hand));
	}

	// The grip space is simple - it's just the hand-to-grip transform matrix
	if (componentName == "handgrip") {
		result->mTrackingToComponentLocal = G2S_m34(handToGripSpace);
		result->mTrackingToComponentRenderModel = G2S_m34(handToGripSpace);
		result->uProperties = VRComponentProperty_IsVisible | VRComponentProperty_IsStatic;
		return true;
	}

	// If it's not manually specified, calculate the 'tip' position to match up with the OpenXR aim pose.
	if (componentName != "tip") {
		return false;
	}

	// Input may not have been made yet (a game setting up its models before its first frame)
	std::shared_ptr<BaseInput> input = GetBaseInput();
	if (!input)
		return false;
	XrSpace componentSpace, gripSpace;
	input->GetHandSpace(hand, componentSpace, true);
	input->GetHandSpace(hand, gripSpace, false);

	if (!componentSpace || !gripSpace) {
		return false;
	}

	XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };
	if (!xr_gbl)
		return false;
	OC_XR_SOFT(xrLocateSpace(componentSpace, gripSpace, xr_gbl->GetBestTime(), &location), false);

	if ((location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) == 0) {
		// If the location is invalid, there's not really a lot we can do. Just use the
		// last known position as is currently in pose.
		OOVR_LOG_ONCE("Relative component location is not valid");
	}

	glm::mat4 gripToComponentSpace = X2G_om34_pose(location.pose);
	glm::mat4 handToComponentSpace = handToGripSpace * gripToComponentSpace;

	result->mTrackingToComponentLocal = G2S_m34(handToComponentSpace);
	result->mTrackingToComponentRenderModel = G2S_m34(handToComponentSpace);
	result->uProperties = VRComponentProperty_IsVisible | VRComponentProperty_IsStatic;
	return true;
}

bool BaseRenderModels::RenderModelHasComponent(const char* pchRenderModelName, const char* pchComponentName)
{
	if (pchRenderModelName) {
		if (std::shared_ptr<const RmModel> model = FindModel(pchRenderModelName))
			return FindComponent(*model, pchComponentName) != nullptr;
	}
	// The models' one component is named after the model (GetComponentName)
	return pchRenderModelName && pchComponentName && IsKnownRenderModel(pchRenderModelName)
	    && strcmp(pchRenderModelName, pchComponentName) == 0;
}

uint32_t BaseRenderModels::GetRenderModelThumbnailURL(const char* pchRenderModelName, VR_OUT_STRING() char* pchThumbnailURL, uint32_t unThumbnailURLLen, EVRRenderModelError* peError)
{
	// A SteamVR model: its thumbnail file as a file URL ("" if it names none)
	if (std::shared_ptr<const RmModel> model = pchRenderModelName ? FindModel(pchRenderModelName) : nullptr) {
		if (peError)
			*peError = VRRenderModelError_None;
		string url;
		if (!model->thumbnail.empty())
			url = FileUrl(model->folder / PathFromUtf8(model->thumbnail));
		return CopyOutModelString(url, pchThumbnailURL, unThumbnailURLLen, peError);
	}

	if (peError)
		*peError = pchRenderModelName && IsKnownRenderModel(pchRenderModelName) ? VRRenderModelError_NotSupported : VRRenderModelError_InvalidModel;
	if (pchThumbnailURL && unThumbnailURLLen)
		pchThumbnailURL[0] = 0;
	// OpenComposite's models have no thumbnails
	return 0;
}

uint32_t BaseRenderModels::GetRenderModelOriginalPath(const char* pchRenderModelName, VR_OUT_STRING() char* pchOriginalPath, uint32_t unOriginalPathLen, EVRRenderModelError* peError)
{
	// A SteamVR model: its OBJ in its folder, as SteamVR answers (LoadRenderModel_Async takes it)
	if (std::shared_ptr<const RmModel> model = pchRenderModelName ? FindModel(pchRenderModelName) : nullptr) {
		if (peError)
			*peError = VRRenderModelError_None;
		// An own controller model has no OBJ of the whole: its name is what loads it
		if (IsOwnControllerModel(model->name) && !FindSteamVRRenderModel(pchRenderModelName))
			return CopyOutModelString(model->name, pchOriginalPath, unOriginalPathLen, peError);
		string stem = model->name;
		if (!stem.empty() && stem[0] == '{')
			stem = stem.substr(stem.find('}') + 1);
		std::filesystem::path file = model->folder / PathFromUtf8(stem + ".obj");
		std::u8string native = file.make_preferred().u8string();
		return CopyOutModelString(string(native.begin(), native.end()), pchOriginalPath, unOriginalPathLen, peError);
	}

	if (peError)
		*peError = pchRenderModelName && IsKnownRenderModel(pchRenderModelName) ? VRRenderModelError_NotSupported : VRRenderModelError_InvalidModel;
	if (pchOriginalPath && unOriginalPathLen)
		pchOriginalPath[0] = 0;
	// OpenComposite's models are built into the DLL: no path
	return 0;
}

const char* BaseRenderModels::GetRenderModelErrorNameFromEnum(EVRRenderModelError error)
{
	// As SteamVR answers
	return OC_EnumNameAsSteamVR("EVRRenderModelError", (int32_t)error, "Unknown render model error (%d)", true);
}
