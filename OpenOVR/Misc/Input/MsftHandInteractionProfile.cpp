#include "stdafx.h"

#include "MsftHandInteractionProfile.h"

MsftHandInteractionProfile::MsftHandInteractionProfile()
{
	const char* perHandPaths[] = {
		"/input/select/value",
		"/input/squeeze/value",
		"/input/aim/pose",
		"/input/grip/pose",
	};
	for (const char* str : perHandPaths) {
		validInputPaths.insert("/user/hand/left" + std::string(str));
		validInputPaths.insert("/user/hand/right" + std::string(str));
	}

	// The pinch is the trigger and the grasp the grip, as a binding file for any controller names them
	rawSourceNames = { { "select", "trigger" }, { "squeeze", "grip" } };
}

const std::string& MsftHandInteractionProfile::GetPath() const
{
	static const std::string path = "/interaction_profiles/microsoft/hand_interaction";
	return path;
}

std::optional<const char*> MsftHandInteractionProfile::GetLeftHandRenderModelName() const
{
	return std::nullopt;
}

std::optional<const char*> MsftHandInteractionProfile::GetRightHandRenderModelName() const
{
	return std::nullopt;
}

std::optional<const char*> MsftHandInteractionProfile::GetOpenVRName() const
{
	return std::nullopt;
}

std::optional<vr::EVRSkeletalTrackingLevel> MsftHandInteractionProfile::GetOpenVRTrackinglevel() const
{
	return vr::VRSkeletalTracking_Full;
}

std::string MsftHandInteractionProfile::BindingConversionType() const
{
	return "hand_interaction";
}

const InteractionProfile::LegacyBindings* MsftHandInteractionProfile::GetLegacyBindings(const std::string& handPath) const
{
	static LegacyBindings bindings = {};
	if (!bindings.trigger) {
		bindings.trigger = "input/select/value";
		bindings.triggerClick = "input/select/value";
		bindings.grip = "input/squeeze/value";
		bindings.gripPoseAction = "input/grip/pose";
		bindings.aimPoseAction = "input/aim/pose";
	}
	return &bindings;
}
