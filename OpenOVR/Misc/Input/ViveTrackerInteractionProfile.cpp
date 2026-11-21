#include "ViveTrackerInteractionProfile.h"
#include "logging.h"

ViveTrackerInteractionProfile::ViveTrackerInteractionProfile()
{
	propertiesMap = {
		{ vr::Prop_TrackingSystemName_String, { "lighthouse" } },
		{ vr::Prop_ControllerType_String, { GetOpenVRName().value() } }
	};
}

const std::string& ViveTrackerInteractionProfile::GetPath() const
{
	static std::string path = "/interaction_profiles/htc/vive_tracker_htcx";
	return path;
}

std::optional<const char*> ViveTrackerInteractionProfile::GetLeftHandRenderModelName() const
{
	return GetOpenVRName().value();
}

std::optional<const char*> ViveTrackerInteractionProfile::GetRightHandRenderModelName() const
{
	return GetOpenVRName().value();
}

InteractionProfile::LegacyBindings ViveTrackerInteractionProfile::MakeLegacyBindings(const std::string& handPath) const
{
	// A tracker has none of a controller's inputs
	return {};
}

std::optional<const char*> ViveTrackerInteractionProfile::GetOpenVRName() const
{
	return "vive_tracker";
}

std::optional<vr::EVRSkeletalTrackingLevel> ViveTrackerInteractionProfile::GetOpenVRTrackinglevel() const
{
	return vr::VRSkeletalTracking_Estimated;
}

bool ViveTrackerInteractionProfile::CanHaveBindings() const
{
	// generic trackers do not support any actions, therefore no bindings.
	return false;
}
