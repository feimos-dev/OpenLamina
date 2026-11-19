// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

#include "../DrvOpenXR/DeviceIdentity.h"
#include "Misc/Input/InteractionProfile.h"
#include "Misc/Input/ViveCosmosInteractionProfile.h"
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

	// An OpenVR grip value maps onto the squeeze button
	CHECK(cosmos.TranslateAction("/user/hand/left/input/grip/value") == "/user/hand/left/input/squeeze/click");
	CHECK(cosmos.TranslateAction("/user/hand/left/input/joystick/x") == "/user/hand/left/input/thumbstick/x");
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
