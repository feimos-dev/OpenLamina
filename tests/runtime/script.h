#pragma once
// The stand-in runtime's script (OC_TEST_RUNTIME_SCRIPT) and dump (OC_TEST_RUNTIME_DUMP).
//
// The script is JSON (comments allowed); every key is optional:
//   "system_name": "...", "extensions": ["XR_KHR_D3D11_enable", ...], "view_size": [w, h], "fov_deg": 45,
//   "ipd": 0.063, "display_hz": 90, "stage_bounds": [width, depth],
//   "events":   [{ "frame": 60, "type": "session_state", "state": "LOSS_PENDING" }],
//   "fail":     [{ "function": "xrEndFrame", "result": "XR_ERROR_RUNTIME_FAILURE", "after": 29, "count": 1 }]
//               (the first `after` calls of the function succeed, then `count` fail with `result`; see FAIL_POINT),
//   "interaction_profile": "/interaction_profiles/valve/index_controller",
//   "inputs":   [{ "frame": 5, "path": "/user/hand/right/input/trigger/value", "value": 1.0 }],
//   "presence": [{ "frame": 10, "worn": false }],
//   "gaze":     [{ "frame": 0, "yaw_deg": 10, "pitch_deg": -5, "valid": true, "tracked": true, "position_valid": true }]
//               (XR_EXT_eye_gaze_interaction's gaze from the head, yaw to the right and pitch up; each entry holds from
//               its frame, valid, tracked and position_valid default to true; a gaze without a valid position is at the stage's origin; without entries the gaze looks straight ahead),
//   "cant_deg": 10 (each view turned outwards by this much about the head's up axis),
//   "fov_inner_deg": 40 (each view's half-angle towards the nose, fov_deg the others)

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

struct Script {
	std::string systemName = "OpenComposite test runtime";
	std::set<std::string> extensions = { "XR_KHR_D3D11_enable" };
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
	std::vector<ScriptInput> inputs;
	std::vector<ScriptPresence> presence;
	std::vector<ScriptGaze> gaze;
};

// Loaded once from OC_TEST_RUNTIME_SCRIPT (defaults without it)
const Script& GetScript();

// Returns true (and sets *result) if the script makes this call to function fail
bool ScriptFails(const char* function, XrResult* result);

// The dump, written to OC_TEST_RUNTIME_DUMP at xrDestroyInstance
struct Dump {
	std::mutex lock;
	Json::Value root{ Json::objectValue };
	void Write();
};
Dump& GetDump();
