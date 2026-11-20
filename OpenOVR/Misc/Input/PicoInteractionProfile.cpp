#include "stdafx.h"

#include "PicoInteractionProfile.h"

PicoInteractionProfile::PicoInteractionProfile(Model model)
    : model(model)
{
	// The face buttons by hand, then the inputs both hands have. The system button is left out as on the other
	// profiles: a runtime need not support it, and no OpenVR game can use it.
	const char* leftPaths[] = {
		"/user/hand/left/input/x/click",
		"/user/hand/left/input/x/touch",
		"/user/hand/left/input/y/click",
		"/user/hand/left/input/y/touch",
	};
	const char* rightPaths[] = {
		"/user/hand/right/input/a/click",
		"/user/hand/right/input/a/touch",
		"/user/hand/right/input/b/click",
		"/user/hand/right/input/b/touch",
	};
	const char* perHandPaths[] = {
		"/input/menu/click",
		"/input/squeeze/click",
		"/input/squeeze/value",
		"/input/trigger/click",
		"/input/trigger/value",
		"/input/trigger/touch",
		"/input/thumbstick/x",
		"/input/thumbstick/y",
		"/input/thumbstick",
		"/input/thumbstick/click",
		"/input/thumbstick/touch",
		"/input/grip/pose",
		"/input/aim/pose",
		"/output/haptic",
	};

	// The G3's inputs, on either hand it's held in: only those its profile lists
	const char* g3Paths[] = {
		"/input/trigger/click",
		"/input/trigger/value",
		"/input/menu/click",
		"/input/thumbstick",
		"/input/thumbstick/click",
		"/input/grip/pose",
		"/input/aim/pose",
	};
	if (model == Model::G3) {
		for (const char* str : g3Paths) {
			validInputPaths.insert("/user/hand/left" + std::string(str));
			validInputPaths.insert("/user/hand/right" + std::string(str));
		}
		rawSourceNames = { { "thumbstick", "joystick" }, { "menu", "application_menu" } };
		propertiesMap = { { vr::Prop_ControllerType_String, { GetOpenVRName().value() } } };
		return;
	}

	for (const char* str : leftPaths)
		validInputPaths.insert(str);
	for (const char* str : rightPaths)
		validInputPaths.insert(str);
	for (const char* str : perHandPaths) {
		validInputPaths.insert("/user/hand/left" + std::string(str));
		validInputPaths.insert("/user/hand/right" + std::string(str));
	}

	// SteamVR's names for the raw inputs the binding evaluator reads (RawComponents)
	rawSourceNames = { { "squeeze", "grip" }, { "thumbstick", "joystick" }, { "menu", "application_menu" } };

	propertiesMap = {
		{ vr::Prop_ControllerType_String, { GetOpenVRName().value() } }
	};
}

const std::string& PicoInteractionProfile::GetPath() const
{
	static const std::string neo3 = "/interaction_profiles/bytedance/pico_neo3_controller";
	static const std::string pico4 = "/interaction_profiles/bytedance/pico4_controller";
	static const std::string g3 = "/interaction_profiles/bytedance/pico_g3_controller";
	return model == Model::Neo3 ? neo3 : model == Model::Pico4 ? pico4 : g3;
}

std::optional<const char*> PicoInteractionProfile::GetLeftHandRenderModelName() const
{
	return std::nullopt;
}

std::optional<const char*> PicoInteractionProfile::GetRightHandRenderModelName() const
{
	return std::nullopt;
}

std::optional<const char*> PicoInteractionProfile::GetOpenVRName() const
{
	return "pico_controller";
}

std::optional<vr::EVRSkeletalTrackingLevel> PicoInteractionProfile::GetOpenVRTrackinglevel() const
{
	return vr::VRSkeletalTracking_Estimated;
}

const InteractionProfile::LegacyBindings* PicoInteractionProfile::GetLegacyBindings(const std::string& handPath) const
{
	if (model == Model::G3) {
		// The trigger, and the menu button as SteamVR's menu
		static LegacyBindings g3 = {};
		if (!g3.system) {
			g3.trigger = "input/trigger/value";
			g3.triggerClick = "input/trigger/click";
			g3.stickBtn = "input/thumbstick/click";
			g3.gripPoseAction = "input/grip/pose";
			g3.aimPoseAction = "input/aim/pose";
			g3.system = "input/menu/click";
		}
		return &g3;
	}

	static LegacyBindings allBindings[2] = { {}, {} };
	int hand = handPath == "/user/hand/left" ? vr::Eye_Left : vr::Eye_Right;
	LegacyBindings& bindings = allBindings[hand];

	if (!bindings.menu) {
		bindings = {};
		bindings.stickX = "input/thumbstick/x";
		bindings.stickY = "input/thumbstick/y";
		bindings.stickBtn = "input/thumbstick/click";
		bindings.stickBtnTouch = "input/thumbstick/touch";

		bindings.trigger = "input/trigger/value";
		bindings.triggerClick = "input/trigger/click";
		bindings.triggerTouch = "input/trigger/touch";

		bindings.grip = "input/squeeze/value";

		bindings.haptic = "output/haptic";

		bindings.gripPoseAction = "input/grip/pose";
		bindings.aimPoseAction = "input/aim/pose";

		if (handPath == "/user/hand/left") {
			bindings.menu = "input/y/click";
			bindings.menuTouch = "input/y/touch";
			bindings.btnA = "input/x/click";
			bindings.btnATouch = "input/x/touch";

			// The menu button is SteamVR's menu
			bindings.system = "input/menu/click";
		} else {
			bindings.menu = "input/b/click";
			bindings.menuTouch = "input/b/touch";
			bindings.btnA = "input/a/click";
			bindings.btnATouch = "input/a/touch";
		}
	}

	return &bindings;
}
