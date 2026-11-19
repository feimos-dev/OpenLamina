// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

#include "../DrvOpenXR/DeviceIdentity.h"
#include "Misc/Input/InteractionProfile.h"
#include "Misc/Input/PicoInteractionProfile.h"
#include "Misc/Input/ViveCosmosInteractionProfile.h"
#include "Misc/Input/ViveFocus3InteractionProfile.h"
#include "Misc/xr_ext.h"

namespace {
bool HasProfile(const InteractionProfile::ProfileList& profiles, const std::string& path)
{
	for (const auto& profile : profiles)
		if (profile->GetPath() == path)
			return true;
	return false;
}
} // namespace

TEST_CASE("Extension_profiles_are_offered_only_when_their_extension_is_enabled")
{
	const std::string cosmos = "/interaction_profiles/htc/vive_cosmos_controller";

	XrExt without(0, {});
	CHECK_FALSE(HasProfile(InteractionProfile::CreateProfiles(without), cosmos));
	CHECK(HasProfile(InteractionProfile::CreateProfiles(without), "/interaction_profiles/oculus/touch_controller"));

	XrExt with(0, { "XR_HTC_vive_cosmos_controller_interaction" });
	CHECK(HasProfile(InteractionProfile::CreateProfiles(with), cosmos));

	const std::string focus3 = "/interaction_profiles/htc/vive_focus3_controller";
	CHECK_FALSE(HasProfile(InteractionProfile::CreateProfiles(with), focus3));
	XrExt withFocus3(0, { "XR_HTC_vive_focus3_controller_interaction" });
	CHECK(HasProfile(InteractionProfile::CreateProfiles(withFocus3), focus3));
	CHECK_FALSE(HasProfile(InteractionProfile::CreateProfiles(withFocus3), cosmos));

	// One extension gives the PICO profiles
	const std::string neo3 = "/interaction_profiles/bytedance/pico_neo3_controller";
	const std::string pico4 = "/interaction_profiles/bytedance/pico4_controller";
	CHECK_FALSE(HasProfile(InteractionProfile::CreateProfiles(withFocus3), pico4));
	XrExt withPico(0, { "XR_BD_controller_interaction" });
	CHECK(HasProfile(InteractionProfile::CreateProfiles(withPico), neo3));
	CHECK(HasProfile(InteractionProfile::CreateProfiles(withPico), pico4));
	CHECK_FALSE(HasProfile(InteractionProfile::CreateProfiles(withPico), focus3));
}

TEST_CASE("Vive_cosmos_profile_paths")
{
	ViveCosmosInteractionProfile cosmos;
	CHECK(cosmos.GetPath() == "/interaction_profiles/htc/vive_cosmos_controller");
	CHECK(std::string(*cosmos.GetOpenVRName()) == "vive_cosmos_controller");

	// The face buttons are on one hand each, the menu button is on the left
	CHECK(cosmos.IsInputPathValid("/user/hand/left/input/x/click"));
	CHECK(cosmos.IsInputPathValid("/user/hand/left/input/menu/click"));
	CHECK_FALSE(cosmos.IsInputPathValid("/user/hand/right/input/x/click"));
	CHECK_FALSE(cosmos.IsInputPathValid("/user/hand/right/input/menu/click"));
	CHECK(cosmos.IsInputPathValid("/user/hand/right/input/b/click"));
	CHECK_FALSE(cosmos.IsInputPathValid("/user/hand/left/input/a/click"));

	// The squeeze is a button, with no value
	CHECK(cosmos.IsInputPathValid("/user/hand/left/input/squeeze/click"));
	CHECK_FALSE(cosmos.IsInputPathValid("/user/hand/left/input/squeeze/value"));
	CHECK(cosmos.IsInputPathValid("/user/hand/right/input/trigger/click"));
	CHECK(cosmos.IsInputPathValid("/user/hand/right/input/thumbstick/touch"));
}

TEST_CASE("Vive_cosmos_controller_identity")
{
	ViveCosmosInteractionProfile cosmos;
	DeviceIdentity right = ControllerIdentity(cosmos, vr::TrackedControllerRole_RightHand);
	CHECK(right.controllerType == "vive_cosmos_controller");
	CHECK((right.supportedButtons & vr::ButtonMaskFromId(vr::k_EButton_A)) != 0);
	CHECK((right.supportedButtons & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger)) != 0);
	CHECK((right.supportedButtons & vr::ButtonMaskFromId(vr::k_EButton_Grip)) != 0);
}

TEST_CASE("Vive_focus3_profile_paths")
{
	ViveFocus3InteractionProfile focus3;
	CHECK(focus3.GetPath() == "/interaction_profiles/htc/vive_focus3_controller");
	CHECK(std::string(*focus3.GetOpenVRName()) == "vive_focus3_controller");

	// The squeeze has a value and a touch, the thumbrest a touch
	CHECK(focus3.IsInputPathValid("/user/hand/left/input/squeeze/value"));
	CHECK(focus3.IsInputPathValid("/user/hand/right/input/squeeze/touch"));
	CHECK(focus3.IsInputPathValid("/user/hand/right/input/thumbrest/touch"));
	// The face buttons have no touch, and are on one hand each
	CHECK_FALSE(focus3.IsInputPathValid("/user/hand/right/input/a/touch"));
	CHECK(focus3.IsInputPathValid("/user/hand/left/input/y/click"));
	CHECK_FALSE(focus3.IsInputPathValid("/user/hand/right/input/y/click"));
}

TEST_CASE("Vive_focus3_controller_identity")
{
	ViveFocus3InteractionProfile focus3;
	DeviceIdentity left = ControllerIdentity(focus3, vr::TrackedControllerRole_LeftHand);
	CHECK(left.controllerType == "vive_focus3_controller");
	CHECK((left.supportedButtons & vr::ButtonMaskFromId(vr::k_EButton_ApplicationMenu)) != 0);
}

TEST_CASE("Pico_profiles_have_their_own_paths_and_types")
{
	PicoInteractionProfile neo3(PicoInteractionProfile::Model::Neo3);
	PicoInteractionProfile pico4(PicoInteractionProfile::Model::Pico4);
	CHECK(neo3.GetPath() == "/interaction_profiles/bytedance/pico_neo3_controller");
	CHECK(pico4.GetPath() == "/interaction_profiles/bytedance/pico4_controller");
	CHECK(std::string(*neo3.GetOpenVRName()) == "pico_controller");
	CHECK(std::string(*pico4.GetOpenVRName()) == "pico_controller");

	// The menu button is on both hands, the face buttons have a touch, the squeeze a value and a click
	CHECK(pico4.IsInputPathValid("/user/hand/left/input/menu/click"));
	CHECK(pico4.IsInputPathValid("/user/hand/right/input/menu/click"));
	CHECK(pico4.IsInputPathValid("/user/hand/right/input/a/touch"));
	CHECK_FALSE(pico4.IsInputPathValid("/user/hand/left/input/a/touch"));
	CHECK(pico4.IsInputPathValid("/user/hand/left/input/squeeze/click"));
	CHECK(pico4.IsInputPathValid("/user/hand/left/input/squeeze/value"));
	CHECK_FALSE(pico4.IsInputPathValid("/user/hand/left/input/thumbrest/touch"));
	CHECK(neo3.GetValidInputPaths() == pico4.GetValidInputPaths());
}
