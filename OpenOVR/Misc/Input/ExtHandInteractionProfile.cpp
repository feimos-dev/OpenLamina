#include "stdafx.h"

#include "ExtHandInteractionProfile.h"

ExtHandInteractionProfile::ExtHandInteractionProfile(std::unique_ptr<InteractionProfile> emulatedProfile, bool bindable)
    : emulated(std::move(emulatedProfile)), bindable(bindable)
{
	// The profile's inputs, as XR_EXT_hand_interaction lists them
	const char* perHandPaths[] = {
		"/input/pinch_ext/pose",
		"/input/poke_ext/pose",
		"/input/grip/pose",
		"/input/aim/pose",
		"/input/pinch_ext/value",
		"/input/pinch_ext/ready_ext",
		"/input/grasp_ext/value",
		"/input/grasp_ext/ready_ext",
		"/input/aim_activate_ext/value",
		"/input/aim_activate_ext/ready_ext",
	};
	for (const char* str : perHandPaths) {
		validInputPaths.insert("/user/hand/left" + std::string(str));
		validInputPaths.insert("/user/hand/right" + std::string(str));
	}

	// The pinch is the trigger and the grasp the grip; an Index controller's grip is also read by its force
	rawSourceNames = { { "pinch_ext", "trigger" }, { "grasp_ext", "grip" } };
	for (const char* hand : { "/user/hand/left", "/user/hand/right" })
		extraRawComponents.emplace_back(std::string(hand) + "/input/grasp_ext/value", std::string(hand) + "/input/grip/force");

	// The rest of the controller shown: its properties, reference poses and components, but the aim pose is the
	// pointer (its tip)
	CopyControllerFrom(*emulated);
	leftComponentTransforms.erase("tip");
	rightComponentTransforms.erase("tip");
}

const std::string& ExtHandInteractionProfile::GetPath() const
{
	static const std::string path = "/interaction_profiles/ext/hand_interaction_ext";
	return path;
}

std::optional<const char*> ExtHandInteractionProfile::GetLeftHandRenderModelName() const
{
	return emulated->GetLeftHandRenderModelName();
}

std::optional<const char*> ExtHandInteractionProfile::GetRightHandRenderModelName() const
{
	return emulated->GetRightHandRenderModelName();
}

std::optional<const char*> ExtHandInteractionProfile::GetOpenVRName() const
{
	return emulated->GetOpenVRName();
}

std::optional<vr::EVRSkeletalTrackingLevel> ExtHandInteractionProfile::GetOpenVRTrackinglevel() const
{
	// The fingers are the hand's own
	return vr::VRSkeletalTracking_Full;
}

const InteractionProfile::LegacyBindings* ExtHandInteractionProfile::GetLegacyBindings(const std::string& handPath) const
{
	static LegacyBindings bindings = {};
	if (!bindings.trigger) {
		bindings.trigger = "input/pinch_ext/value";
		bindings.triggerClick = "input/pinch_ext/value";
		bindings.grip = "input/grasp_ext/value";
		bindings.gripPoseAction = "input/grip/pose";
		bindings.aimPoseAction = "input/aim/pose";
	}
	return &bindings;
}
