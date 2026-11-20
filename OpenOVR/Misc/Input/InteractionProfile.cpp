//
// Created by ZNix on 27/02/2021.
//

#include "stdafx.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <utility>

#include "HolographicInteractionProfile.h"
#include "IndexControllerInteractionProfile.h"
#include "InteractionProfile.h"
#include "KhrSimpleInteractionProfile.h"
#include "MsftHandInteractionProfile.h"
#include "OculusInteractionProfile.h"
#include "ReverbG2InteractionProfile.h"
#include "ViveCosmosInteractionProfile.h"
#include "ViveFocus3InteractionProfile.h"
#include "PicoInteractionProfile.h"
#include "ViveInteractionProfile.h"
#include "ViveTrackerInteractionProfile.h"

#include "OculusHandPoses.h"

#include "Misc/Config.h"
#include "Reimpl/BaseInput.h"
#include "generated/static_bases.gen.h"

std::vector<std::pair<std::string, std::string>> InteractionProfile::RawComponents() const
{
	// Every /user/hand/<hand>/input/<source>/<component> path: a parent (/input/thumbstick) or a pose isn't a raw input
	static const std::set<std::string> components = { "click", "touch", "value", "force", "x", "y" };
	std::vector<std::pair<std::string, std::string>> raw;
	for (const std::string& path : validInputPaths) {
		size_t input = path.find("/input/");
		if (input == std::string::npos || !path.starts_with("/user/hand/"))
			continue;
		size_t sourceStart = input + strlen("/input/");
		size_t slash = path.find('/', sourceStart);
		if (slash == std::string::npos || path.find('/', slash + 1) != std::string::npos)
			continue;
		std::string source = path.substr(sourceStart, slash - sourceStart);
		std::string component = path.substr(slash + 1);
		if (!components.count(component))
			continue;
		auto renamed = rawSourceNames.find(source);
		const std::string& steamvrSource = renamed == rawSourceNames.end() ? source : renamed->second;
		raw.emplace_back(path, path.substr(0, sourceStart) + steamvrSource + "/" + component);
	}
	std::sort(raw.begin(), raw.end());
	return raw;
}

const std::unordered_set<std::string>& InteractionProfile::GetValidInputPaths() const
{
	return validInputPaths;
}

bool InteractionProfile::IsInputPathValid(const std::string& inputPath) const
{
	return validInputPaths.find(inputPath) != validInputPaths.end();
}

void InteractionProfile::AddLegacyBindings(const LegacyControllerActions& ctrl, std::vector<XrActionSuggestedBinding>& bindings) const
{
	auto create = [&](XrAction action, const char* path) {
		// Not supported on this controller?
		if (path == nullptr)
			return;

		// Bind the action to a suggested binding
		std::string realPath = ctrl.handPath + "/" + path;
		if (!IsInputPathValid(realPath)) {
			// Only if an input profile gets its paths wrong: that binding is left out
			OOVR_LOGF("Found legacy input path %s, not supported by profile", realPath.c_str());
			return;
		}

		// The action must have been defined (it isn't if OpenXR couldn't make it): that binding is left out
		if (action == XR_NULL_HANDLE)
			return;

		XrActionSuggestedBinding binding = {};
		binding.action = action;

		if (XR_FAILED(xrStringToPath(xr_instance, realPath.c_str(), &binding.binding)))
			return;

		bindings.push_back(binding);
	};

	const LegacyBindings* paths = GetLegacyBindings(ctrl.handPath);

	create(ctrl.system, paths->system);
	// Only when it opens the dashboard: a runtime may refuse a profile's bindings over this path
	if (paths->dashboard && ctrl.dashboard != XR_NULL_HANDLE && oovr_global_configuration.DashboardButton().starts_with("system")) {
		XrActionSuggestedBinding binding = {};
		binding.action = ctrl.dashboard;
		if (XR_SUCCEEDED(xrStringToPath(xr_instance, (ctrl.handPath + "/" + paths->dashboard).c_str(), &binding.binding)))
			bindings.push_back(binding);
	}
	create(ctrl.menu, paths->menu);
	create(ctrl.menuTouch, paths->menuTouch);
	create(ctrl.btnA, paths->btnA);
	create(ctrl.btnATouch, paths->btnATouch);
	create(ctrl.trackpadX, paths->trackpadX);
	create(ctrl.trackpadY, paths->trackpadY);
	create(ctrl.trackpadTouch, paths->trackpadTouch);
	create(ctrl.trackpadForce, paths->trackpadForce);
	create(ctrl.stickX, paths->stickX);
	create(ctrl.stickY, paths->stickY);
	create(ctrl.stickBtn, paths->stickBtn);
	create(ctrl.stickBtnTouch, paths->stickBtnTouch);
	create(ctrl.trigger, paths->trigger);
	create(ctrl.triggerTouch, paths->triggerTouch);
	create(ctrl.triggerClick, paths->triggerClick);
	create(ctrl.triggerProximity, paths->triggerProximity);
	create(ctrl.thumbProximity, paths->thumbProximity);
	create(ctrl.triggerCurl, paths->triggerCurl);
	create(ctrl.grip, paths->grip);
	create(ctrl.gripClick, paths->grip);
	create(ctrl.haptic, paths->haptic);
	create(ctrl.gripPoseAction, paths->gripPoseAction);
	create(ctrl.aimPoseAction, paths->aimPoseAction);
}

InteractionProfile::ProfileList InteractionProfile::CreateProfiles(XrExt& ext)
{
	ProfileList profiles;
	if (ext.G2Controller_Available())
		profiles.emplace_back(std::make_unique<ReverbG2InteractionProfile>());

	profiles.emplace_back(std::make_unique<HolographicInteractionProfile>());
	if (ext.ExtensionEnabled(XR_EXT_SAMSUNG_ODYSSEY_CONTROLLER_EXTENSION_NAME))
		profiles.emplace_back(std::make_unique<HolographicInteractionProfile>(HolographicInteractionProfile::Model::Odyssey));
	profiles.emplace_back(std::make_unique<IndexControllerInteractionProfile>());
	profiles.emplace_back(std::make_unique<ViveWandInteractionProfile>());
	using Touch = OculusTouchInteractionProfile;
	profiles.emplace_back(std::make_unique<Touch>(Touch::Model::Touch, ext.ExtensionEnabled(OC_XR_FB_TOUCH_CONTROLLER_PROXIMITY_EXTENSION_NAME)));
	if (ext.ExtensionEnabled(OC_XR_FB_TOUCH_CONTROLLER_PRO_EXTENSION_NAME))
		profiles.emplace_back(std::make_unique<Touch>(Touch::Model::TouchPro));
	if (ext.ExtensionEnabled(OC_XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME))
		profiles.emplace_back(std::make_unique<Touch>(Touch::Model::TouchPlus));
	if (ext.ExtensionEnabled(OC_XR_HTC_VIVE_COSMOS_CONTROLLER_INTERACTION_EXTENSION_NAME))
		profiles.emplace_back(std::make_unique<ViveCosmosInteractionProfile>());
	if (ext.ExtensionEnabled(OC_XR_HTC_VIVE_FOCUS3_CONTROLLER_INTERACTION_EXTENSION_NAME))
		profiles.emplace_back(std::make_unique<ViveFocus3InteractionProfile>());
	if (ext.ExtensionEnabled(OC_XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME)) {
		profiles.emplace_back(std::make_unique<PicoInteractionProfile>(PicoInteractionProfile::Model::Neo3));
		profiles.emplace_back(std::make_unique<PicoInteractionProfile>(PicoInteractionProfile::Model::Pico4));
		profiles.emplace_back(std::make_unique<PicoInteractionProfile>(PicoInteractionProfile::Model::G3));
	}
	if (ext.ExtensionEnabled(XR_MSFT_HAND_INTERACTION_EXTENSION_NAME))
		profiles.emplace_back(std::make_unique<MsftHandInteractionProfile>());
	profiles.emplace_back(std::make_unique<KhrSimpleInteractionProfile>());
	profiles.emplace_back(std::make_unique<ViveTrackerInteractionProfile>());
	return profiles;
}

const InteractionProfile::ProfileList& InteractionProfile::GetProfileList()
{
	static ProfileList profiles;
	if (profiles.empty())
		profiles = CreateProfiles(*xr_ext);
	return profiles;
}

InteractionProfile* InteractionProfile::GetProfileByPath(const string& name)
{
	static std::map<std::string, InteractionProfile*> byPath;
	if (byPath.empty()) {
		for (const std::unique_ptr<InteractionProfile>& profile : GetProfileList()) {
			byPath[profile->GetPath()] = profile.get();
		}
	}
	if (!byPath.contains(name)) {
		OOVR_LOGF("Could not find interaction profile '%s'", name.c_str());
		return nullptr;
	}
	return byPath.at(name);
}

glm::mat4 InteractionProfile::GetGripToSteamVRTransform(ITrackedDevice::TrackedDeviceType hand) const
{
	if (hand == ITrackedDevice::TrackedDeviceType::HAND_LEFT) {
		return leftHandGripTransform;
	} else if (hand == ITrackedDevice::TrackedDeviceType::HAND_RIGHT) {
		return rightHandGripTransform;
	}
	return glm::identity<glm::mat4>();
}

std::optional<glm::mat4> InteractionProfile::GetComponentTransform(ITrackedDevice::TrackedDeviceType hand, const std::string& name) const
{
	const std::unordered_map<std::string, glm::mat4>& transforms = hand == ITrackedDevice::HAND_RIGHT ? rightComponentTransforms : leftComponentTransforms;
	const auto iter = transforms.find(name);
	if (iter == transforms.end())
		return {};
	else
		return iter->second;
}

std::optional<std::array<vr::VRBoneTransform_t, 31>> InteractionProfile::GetDefaultSkeletalReferencePose(ITrackedDevice::TrackedDeviceType hand, int pose)
{
	using PoseMap = std::unordered_map<int, std::array<vr::VRBoneTransform_t, 31>>;
	static const PoseMap leftPoses = {
		{ VRSkeletalReferencePose_BindPose, oculus::leftBindPose },
		{ VRSkeletalReferencePose_OpenHand, oculus::leftOpenHandPose },
		{ VRSkeletalReferencePose_Fist, oculus::leftFistPose },
		{ VRSkeletalReferencePose_GripLimit, oculus::leftGripLimitPose },
	};
	static const PoseMap rightPoses = {
		{ VRSkeletalReferencePose_BindPose, oculus::rightBindPose },
		{ VRSkeletalReferencePose_OpenHand, oculus::rightOpenHandPose },
		{ VRSkeletalReferencePose_Fist, oculus::rightFistPose },
		{ VRSkeletalReferencePose_GripLimit, oculus::rightGripLimitPose },
	};

	if (hand != ITrackedDevice::HAND_LEFT && hand != ITrackedDevice::HAND_RIGHT)
		return {};

	const PoseMap& poses = hand == ITrackedDevice::HAND_LEFT ? leftPoses : rightPoses;
	const auto iter = poses.find(pose);
	if (iter == poses.end())
		return {};
	return iter->second;
}

std::optional<std::array<vr::VRBoneTransform_t, 31>> InteractionProfile::GetSkeletalReferencePose(ITrackedDevice::TrackedDeviceType hand, int pose) const
{
	const auto& poses = hand == ITrackedDevice::HAND_LEFT ? leftHandPoses : rightHandPoses;

	// Fall back to oculus poses, which should be close enough for most controllers
	if (poses.empty()) {
		OOVR_LOG_ONCE("WARNING: No reference poses defined for interaction profile, using fallback.");
		return GetDefaultSkeletalReferencePose(hand, pose);
	}

	const auto iter = poses.find(pose);
	if (iter == poses.end())
		return {};
	else
		return iter->second;
}
