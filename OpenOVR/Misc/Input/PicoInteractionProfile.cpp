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

	pathTranslationMap = {
		{ "grip", "squeeze" },
		{ "joystick", "thumbstick" },
		{ "pull", "value" },
		{ "grip/force", "squeeze/value" },
		{ "application_menu", "menu" }
	};

	propertiesMap = {
		{ vr::Prop_ControllerType_String, { GetOpenVRName().value() } }
	};
}

const std::string& PicoInteractionProfile::GetPath() const
{
	static std::string neo3 = "/interaction_profiles/bytedance/pico_neo3_controller";
	static std::string pico4 = "/interaction_profiles/bytedance/pico4_controller";
	return model == Model::Neo3 ? neo3 : pico4;
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
	return model == Model::Neo3 ? "pico_neo3_controller" : "pico4_controller";
}

std::optional<vr::EVRSkeletalTrackingLevel> PicoInteractionProfile::GetOpenVRTrackinglevel() const
{
	return vr::VRSkeletalTracking_Estimated;
}

const InteractionProfile::LegacyBindings* PicoInteractionProfile::GetLegacyBindings(const std::string& handPath) const
{
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
