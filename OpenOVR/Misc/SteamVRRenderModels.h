#pragma once

// SteamVR's render models, read from the user's SteamVR install: <steamvr>/resources/rendermodels/<name>/, and a
// driver's at <steamvr>/drivers/<driver>/resources/rendermodels/<name>/ (named "{<driver>}<name>").
//
// A model's folder holds <name>.obj (the whole model) and, optionally, <name>.json, whose "components" name the parts
// that move or that games attach things to: each with its own OBJ ("filename", in the model's coordinates), its local
// frame ("component_local": "origin" and "rotate_xyz" in degrees) and how it moves ("motion"). An OBJ's material
// library names its texture (map_Kd); textures are read as PNG.

#include "Misc/Image.h"
#include "generated/interfaces/deleted_types.h"
#include "json/json.h"

#include <openxr/openxr.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The vertex OpenVR's render models use (position and normal in metres in the device's space, then UV)
using RmVertex = vr::IVRRenderModels_001::RenderModel_Vertex_t;

struct RmComponent {
	std::string name;
	std::string objFile; // relative to the model's folder; empty for a component with nothing to draw (tip, base)
	XrPosef local; // component_local, in the model's space
	std::string motionType; // "rotate", "translate", "trackpad", "joystick"; empty for a static component
	Json::Value motion; // the whole "motion" object
	Json::Value visibility; // the "visibility" object (null: always visible)
};

struct RmModel {
	std::string name;
	std::vector<RmComponent> components;
	std::filesystem::path folder;
	std::string thumbnail; // the "thumbnail" file, relative to the folder; empty for none
};

// A model's (or one component's) triangles, ready for RenderModel_t
struct RmMesh {
	std::vector<RmVertex> vertices;
	std::vector<uint16_t> indices; // three per triangle
	std::filesystem::path diffuse; // the texture file, empty for none
};

/** The model names under <steamvr>/resources/rendermodels (each folder), then each driver's as "{<driver>}<name>",
 * sorted. Empty if there are none or the folders can't be read. */
std::vector<std::string> ListSteamVRRenderModels(const std::filesystem::path& steamvr);

/** The folder of a model name (plain or "{<driver>}<name>"); empty for a name that can't be one (path separators,
 * "..", an unclosed brace). The folder needn't exist. */
std::filesystem::path SteamVRRenderModelFolder(const std::filesystem::path& steamvr, const std::string& name);

/** A model and its components. nullopt if its folder doesn't exist, if it has neither <name>.json nor <name>.obj,
 * or if its JSON can't be read or isn't a model's (components that aren't an object, a component that isn't one).
 * Never throws. */
std::optional<RmModel> LoadSteamVRRenderModel(const std::filesystem::path& steamvr, const std::string& name);

/** Reads an OBJ file's triangles (faces of more than three corners as fans; negative indices count back), one vertex
 * for each distinct position/UV/normal corner. v is turned into 1 - v: OBJ's v starts at the texture's bottom row,
 * a RenderModel_TextureMap_t's first row is its top.
 * diffuse, if given, gets the map_Kd of the first material used, as a path relative to the OBJ's folder ("" for none).
 * False for a file that can't be read, has no triangles, has a corner that points nowhere, or needs more than
 * 65535 vertices; nothing is appended then. */
bool LoadObj(const std::filesystem::path& obj, std::vector<RmVertex>& verts, std::vector<uint16_t>& indices, std::string* diffuse);

/** The triangles of a model: its component's OBJ, or with an empty component name <name>.obj, else (if there is
 * none) every component with an OBJ that's visible by default, together. nullopt if there's nothing to draw. */
std::optional<RmMesh> LoadRenderModelMesh(const RmModel& model, const std::string& component);

/** A PNG file as RGBA8, through LoadImageFile. nullopt (logged) for a file it refuses. */
std::optional<Image> LoadRenderModelTexture(const std::filesystem::path& file);

/** The image at another size, each pixel the nearest of the original's. An empty image if either size is 0 or the
 * original is empty. */
Image ResampleImage(const Image& image, uint32_t width, uint32_t height);

/** The image and the levels below it, each half the size of the one before (at least 1 pixel, each pixel the average of
 * the four above it). levels is how many to make, 0 for all the way down to 1x1; never more than that, never fewer than 1. */
std::vector<Image> BuildMipChain(const Image& base, uint32_t levels);

/** Where a component's geometry is moved to for the current input, in the model's space (identity at rest):
 * "rotate" turns it about "axis" through "pivot" by value_mapping's degrees at value (0 to 1), "translate" moves it
 * along "axis" by value_mapping's metres; "trackpad" tilts about its "center" by press_rotation_x/y (as far as value
 * presses it) and moves by press_translate times value and by touch_translate_x/y at the touch point; "joystick"
 * tilts by joystick_rotation_x/y at axis and moves by press_translate times value. axis is -1 to 1 on each side.
 * Other types (and none) are static. */
XrPosef ComponentPose(const RmComponent& c, float value, vr::HmdVector2_t axis);

/** Whether a component is drawn: "visibility"'s "default" (true without one), else its "touch" or "press" entry
 * while touched or pressed */
bool ComponentVisible(const RmComponent& c, bool touched, bool pressed);

/** The models of the SteamVR install FindSteamVRInstall() finds, cached by name for the process (a failure too).
 * Null without SteamVR or for a model that can't be loaded. Thread-safe. */
std::shared_ptr<const RmModel> FindSteamVRRenderModel(const std::string& name);

/** The SteamVR install's model names (ListSteamVRRenderModels), read once. Empty without SteamVR. */
const std::vector<std::string>& SteamVRRenderModelNames();
