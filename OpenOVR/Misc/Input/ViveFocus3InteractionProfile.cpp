#include "stdafx.h"

#include "ViveFocus3InteractionProfile.h"

ViveFocus3InteractionProfile::ViveFocus3InteractionProfile()
{
	// The face buttons by hand, then the inputs both hands have. The system button is left out as on the other
	// profiles: a runtime need not support it, and no OpenVR game can use it.
	const char* leftPaths[] = {
		"/user/hand/left/input/x/click",
		"/user/hand/left/input/y/click",
		"/user/hand/left/input/menu/click",
	};
	const char* rightPaths[] = {
		"/user/hand/right/input/a/click",
		"/user/hand/right/input/b/click",
	};
	const char* perHandPaths[] = {
		"/input/squeeze/click",
		"/input/squeeze/touch",
		"/input/squeeze/value",
		"/input/trigger/click",
		"/input/trigger/value",
		"/input/trigger/touch",
		"/input/thumbstick/x",
		"/input/thumbstick/y",
		"/input/thumbstick",
		"/input/thumbstick/click",
		"/input/thumbstick/touch",
		"/input/thumbrest/touch",
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

	propertiesMap = {
		{ vr::Prop_ControllerType_String, { GetOpenVRName().value() } }
	};
}

const std::string& ViveFocus3InteractionProfile::GetPath() const
{
	static std::string path = "/interaction_profiles/htc/vive_focus3_controller";
	return path;
}

std::optional<const char*> ViveFocus3InteractionProfile::GetLeftHandRenderModelName() const
{
	return std::nullopt;
}

std::optional<const char*> ViveFocus3InteractionProfile::GetRightHandRenderModelName() const
{
	return std::nullopt;
}

std::optional<const char*> ViveFocus3InteractionProfile::GetOpenVRName() const
{
	return "vive_focus3_controller";
}

std::optional<vr::EVRSkeletalTrackingLevel> ViveFocus3InteractionProfile::GetOpenVRTrackinglevel() const
{
	return vr::VRSkeletalTracking_Estimated;
}

const InteractionProfile::LegacyBindings* ViveFocus3InteractionProfile::GetLegacyBindings(const std::string& handPath) const
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
			bindings.btnA = "input/x/click";

			// The menu button is SteamVR's menu
			bindings.system = "input/menu/click";
		} else {
			bindings.menu = "input/b/click";
			bindings.btnA = "input/a/click";
		}
	}

	return &bindings;
}
