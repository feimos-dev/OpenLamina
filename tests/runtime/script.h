#pragma once
// The stand-in runtime's script (OC_TEST_RUNTIME_SCRIPT) and dump (OC_TEST_RUNTIME_DUMP).
//
// The script is JSON (comments allowed); every key is optional:
//   "system_name": "...", "extensions": ["XR_KHR_D3D11_enable", ...], "view_size": [w, h], "fov_deg": 45,
//               (on Linux the D3D extensions are left out, a script left with no graphics API gets the default one,
//               and XR_KHR_win32_convert_performance_counter_time is XR_KHR_convert_timespec_time),
//   "ipd": 0.063, "display_hz": 90, "stage_bounds": [width, depth],
//   "events":   [{ "frame": 60, "type": "session_state", "state": "LOSS_PENDING" }]
//               (or { "frame": 10, "type": "refresh_rate", "rate": 120 }: the display changes to that rate, announced
//               with XrEventDataDisplayRefreshRateChangedFB),
//   "fail":     [{ "function": "xrEndFrame", "result": "XR_ERROR_RUNTIME_FAILURE", "after": 29, "count": 1 }]
//               (the first `after` calls of the function succeed, then `count` fail with `result`; see FAIL_POINT),
//   "interaction_profile": "/interaction_profiles/valve/index_controller",
//   "inputs":   [{ "frame": 5, "path": "/user/hand/right/input/trigger/value", "value": 1.0 }],
//   "presence": [{ "frame": 10, "worn": false }],
//   "gaze":     [{ "frame": 0, "yaw_deg": 10, "pitch_deg": -5, "valid": true, "tracked": true, "position_valid": true }]
//               (XR_EXT_eye_gaze_interaction's gaze from the head, yaw to the right and pitch up; each entry holds from
//               its frame, valid, tracked and position_valid default to true; a gaze without a valid position is at the stage's origin; without entries the gaze looks straight ahead),
//   "cant_deg": 10 (each view turned outwards by this much about the head's up axis),
//   "fov_inner_deg": 40 (each view's half-angle towards the nose, fov_deg the others),
//   "swapchain_formats": [29, 28, 87, 91]   (what xrEnumerateSwapchainFormats returns, in the session's API's enums,
//               and the only formats xrCreateSwapchain then takes; or per API, keyed by session.binding:
//               { "d3d11": [29], "vulkan": [43] }, an API not named keeping its own list. Without it each API's own
//               list, which has no depth or float format, and xrCreateSwapchain also takes the formats the runtime
//               can make that it doesn't list: script the list for a case that looks for depth or float formats),
//   "poses":    { "/user/hand/left/input/aim/pose": { "p": [x, y, z], "q": [x, y, z, w], "v": [x, y, z], "w": [x, y, z],
//               "from_frame": 0 } } (in STAGE; a list of such objects per path takes the last one whose from_frame has
//               been reached; an action space takes the pose of the first path its action is bound to that has one,
//               except a gaze space, which follows "gaze";
//               the velocities are given as they are, always valid: not corrected for poseInActionSpace, nor for the
//               base space's own motion; "/user/head" places the HMD, at (0, 1.7, 0) without it),
//   "refresh_rates": [90]   (XR_FB_display_refresh_rate's rates; without it, display_hz alone),
//   "ipd_change": { "frame": 30, "ipd": 0.07 }   (or a list: the IPD from that frame on),
//   "vive_trackers": [{ "persistent": "/devices/htc/vive_trackerLHR-1", "role": "/user/vive_tracker_htcx/role/waist",
//               "frame": 0 }]   (XR_HTCX_vive_tracker_interaction's trackers, listed from their frame on and announced
//               with XrEventDataViveTrackerConnectedHTCX at it (from frame 1); an empty role is a tracker without one;
//               a tracker's pose is the script's pose of <role>/input/grip/pose),
//   "hand_data_source": 1   (XR_EXT_hand_tracking_data_source's source of the joints: 1 the hand's own, 2 (without
//               it) a controller's)
//
// The dump (only when OC_TEST_RUNTIME_DUMP is set: without it nothing is recorded or read back) keeps whole layer
// records for frames 1-64, every 32nd frame after, and the last frame ended, at most kDumpLayerCap records; the
// others are only counted, in layers_dropped. Each record's samples (frames 1-64 and every 32nd) are the bytes the
// image stores, as RGBA8: an sRGB format gives its sRGB-encoded bytes and a UNORM one its linear bytes, with no
// conversion between them; float and 10-bit channels are scaled to 8 bits (read the record's format to interpret
// them).

#include <openxr/openxr.h>

#include <json/json.h>

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

struct ScriptEvent {
	uint64_t frame = 0;
	std::string type; // "session_state"
	std::string state; // "LOSS_PENDING", "EXITING", "STOPPING", ...
	float rate = 0; // "refresh_rate"
};

struct ScriptFailure {
	std::string function;
	XrResult result = XR_ERROR_RUNTIME_FAILURE;
	int count = 1;
	int after = 0; // the first `after` calls succeed
};

struct ScriptInput {
	uint64_t frame = 0;
	std::string path;
	float value = 0;
};

struct ScriptPresence {
	uint64_t frame = 0;
	bool worn = true;
};

struct ScriptGaze {
	uint64_t frame = 0;
	float yawDeg = 0, pitchDeg = 0;
	bool valid = true;
	bool tracked = true;
	bool positionValid = true;
};

struct ScriptPose {
	uint64_t fromFrame = 0;
	XrPosef pose{ { 0, 0, 0, 1 }, { 0, 0, 0 } };
	XrVector3f linear{ 0, 0, 0 };
	XrVector3f angular{ 0, 0, 0 };
};

struct ScriptViveTracker {
	uint64_t frame = 0;
	std::string persistent, role;
};

struct ScriptIpd {
	uint64_t frame = 0;
	float ipd = 0.063f;
};

struct Script {
	std::string systemName = "OpenComposite test runtime";
	// Without a script: the API OpenLamina makes its own session on (D3D11 on Windows, Vulkan elsewhere)
#ifdef _WIN32
	std::set<std::string> extensions = { "XR_KHR_D3D11_enable" };
#else
	std::set<std::string> extensions = { "XR_KHR_vulkan_enable" };
#endif
	uint32_t viewWidth = 1920, viewHeight = 1920;
	float fovDeg = 45;
	float ipd = 0.063f;
	float cantDeg = 0;
	float fovInnerDeg = -1; // fov_deg
	float displayHz = 90;
	float stageWidth = 2.0f, stageDepth = 3.0f;
	std::vector<ScriptEvent> events;
	std::vector<ScriptFailure> failures;
	std::string interactionProfile;
	std::vector<std::string> extraBoundSources; // listed after every action's own bound sources
	std::vector<ScriptInput> inputs;
	std::vector<ScriptPresence> presence;
	std::vector<ScriptGaze> gaze;
	std::vector<int64_t> swapchainFormats; // empty: the API's own list
	std::map<std::string, std::vector<int64_t>> swapchainFormatsByApi; // the per-API form, keyed by session.binding
	std::map<std::string, std::vector<ScriptPose>> poses;
	std::vector<float> refreshRates; // empty: displayHz alone
	std::vector<ScriptIpd> ipdChanges;
	std::vector<ScriptViveTracker> viveTrackers;
	int32_t handDataSource = 2;
};

// The scripted pose of a path at a frame (the last entry whose from_frame has been reached), nullptr if none
const ScriptPose* ScriptPoseAt(const std::string& path, uint64_t frame);
// The IPD at a frame (ipd, then any ipd_change reached)
float ScriptIpdAt(uint64_t frame);

// Loaded once from OC_TEST_RUNTIME_SCRIPT (defaults without it)
const Script& GetScript();

// The script's swapchain formats for a session.binding (the per-API list, else the flat one); nullptr if it has none
const std::vector<int64_t>* ScriptSwapchainFormats(const std::string& binding);

// Returns true (and sets *result) if the script makes this call to function fail
bool ScriptFails(const char* function, XrResult* result);

// The most layer records the dump keeps (a backstop for a long, unpaced frame loop)
constexpr Json::ArrayIndex kDumpLayerCap = 4096;

// The dump, written to OC_TEST_RUNTIME_DUMP at xrDestroyInstance
struct Dump {
	std::mutex lock;
	Json::Value root{ Json::objectValue };
	// The last frame's layer records when its frame isn't one root keeps: appended to layers by Write
	Json::Value lastFrame{ Json::arrayValue };
	static bool Enabled(); // OC_TEST_RUNTIME_DUMP is set
	void Write();
};
Dump& GetDump();
