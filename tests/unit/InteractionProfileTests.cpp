// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

#include "../DrvOpenXR/DeviceIdentity.h"
#include "Misc/Input/Bindings/Conversion.h"
#include "Misc/Input/ExtHandInteractionProfile.h"
#include "Misc/Input/HolographicInteractionProfile.h"
#include "Misc/Input/IndexControllerInteractionProfile.h"
#include "Misc/Input/KhrSimpleInteractionProfile.h"
#include "Misc/Input/InteractionProfile.h"
#include "Misc/Input/MsftHandInteractionProfile.h"
#include "Misc/Input/OculusInteractionProfile.h"
#include "Misc/Input/PicoInteractionProfile.h"
#include "Misc/Input/ViveCosmosInteractionProfile.h"
#include "Misc/Input/ViveFocus3InteractionProfile.h"
#include "Misc/Input/ViveInteractionProfile.h"
#include "Misc/Input/ViveTrackerInteractionProfile.h"
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

	// The menu button is on both of the Neo 3's hands and on the PICO 4's left; the face buttons have a touch, the
	// squeeze a value and a click
	CHECK(neo3.IsInputPathValid("/user/hand/right/input/menu/click"));
	CHECK(pico4.IsInputPathValid("/user/hand/left/input/menu/click"));
	CHECK_FALSE(pico4.IsInputPathValid("/user/hand/right/input/menu/click"));
	CHECK(pico4.IsInputPathValid("/user/hand/right/input/a/touch"));
	CHECK_FALSE(pico4.IsInputPathValid("/user/hand/left/input/a/touch"));
	CHECK(pico4.IsInputPathValid("/user/hand/left/input/squeeze/click"));
	CHECK(pico4.IsInputPathValid("/user/hand/left/input/squeeze/value"));
	CHECK_FALSE(pico4.IsInputPathValid("/user/hand/left/input/thumbrest/touch"));
	std::unordered_set<std::string> neo3Paths = neo3.GetValidInputPaths();
	neo3Paths.erase("/user/hand/right/input/menu/click");
	CHECK(neo3Paths == pico4.GetValidInputPaths());
}

// Programs tell Lighthouse controllers from Oculus ones by the tracking system, as SteamVR names it (Skyrim VR loads
// its Oculus Touch controls for "oculus"): "lighthouse" for the devices SteamVR's lighthouse driver drives
TEST_CASE("Lighthouse_devices_name_their_tracking_system_as_SteamVR_does")
{
	using enum ITrackedDevice::TrackedDeviceType;
	IndexControllerInteractionProfile index;
	ViveWandInteractionProfile wand;
	ViveTrackerInteractionProfile tracker;
	OculusTouchInteractionProfile touch;
	for (const InteractionProfile* profile : { (const InteractionProfile*)&index, (const InteractionProfile*)&wand }) {
		CHECK(profile->GetProperty<std::string>(vr::Prop_TrackingSystemName_String, HAND_LEFT) == "lighthouse");
		CHECK(profile->GetProperty<std::string>(vr::Prop_TrackingSystemName_String, HAND_RIGHT) == "lighthouse");
		CHECK(profile->GetProperty<std::string>(vr::Prop_TrackingSystemName_String, HAND_NONE) == "lighthouse");
	}
	CHECK(tracker.GetProperty<std::string>(vr::Prop_TrackingSystemName_String, HAND_LEFT) == "lighthouse");
	CHECK(touch.GetProperty<std::string>(vr::Prop_TrackingSystemName_String, HAND_LEFT) == "oculus");
}

TEST_CASE("More_extension_profiles_only_with_their_extension")
{
	const std::string pro = "/interaction_profiles/facebook/touch_controller_pro";
	const std::string plus = "/interaction_profiles/meta/touch_controller_plus";
	const std::string odyssey = "/interaction_profiles/samsung/odyssey_controller";
	const std::string hands = "/interaction_profiles/microsoft/hand_interaction";
	const std::string g3 = "/interaction_profiles/bytedance/pico_g3_controller";

	XrExt without(0, {});
	for (const std::string& path : { pro, plus, odyssey, hands, g3 })
		CHECK_FALSE(HasProfile(InteractionProfile::CreateProfiles(without), path));

	XrExt with(0, { "XR_FB_touch_controller_pro", "XR_META_touch_controller_plus", "XR_EXT_samsung_odyssey_controller",
	                  "XR_MSFT_hand_interaction", "XR_BD_controller_interaction" });
	for (const std::string& path : { pro, plus, odyssey, hands, g3 })
		CHECK(HasProfile(InteractionProfile::CreateProfiles(with), path));
}

TEST_CASE("Touch_profiles_share_the_OpenVR_name_and_add_their_inputs")
{
	using Touch = OculusTouchInteractionProfile;
	Touch touch, withProximity(Touch::Model::Touch, true), pro(Touch::Model::TouchPro), plus(Touch::Model::TouchPlus);
	for (const Touch* t : { &touch, &withProximity, &pro, &plus })
		CHECK(std::string(*t->GetOpenVRName()) == "oculus_touch");
	CHECK(pro.GetPath() == "/interaction_profiles/facebook/touch_controller_pro");
	CHECK(plus.GetPath() == "/interaction_profiles/meta/touch_controller_plus");

	// The finger proximity only with its extension
	CHECK_FALSE(touch.IsInputPathValid("/user/hand/left/input/thumb_fb/proximity_fb"));
	CHECK(withProximity.IsInputPathValid("/user/hand/left/input/thumb_fb/proximity_fb"));
	CHECK(withProximity.IsInputPathValid("/user/hand/right/input/trigger/proximity_fb"));

	// The Pro's thumbrest force, stylus, trigger curl and slide, and trigger haptic; the Plus's names its own
	for (const char* path : { "/input/thumbrest/force", "/input/stylus_fb/force", "/input/trigger/curl_fb", "/input/trigger/slide_fb",
	         "/output/haptic_trigger_fb", "/input/trigger/proximity_fb" }) {
		CHECK(pro.IsInputPathValid(std::string("/user/hand/right") + path));
		CHECK_FALSE(plus.IsInputPathValid(std::string("/user/hand/right") + path));
	}
	for (const char* path : { "/input/trigger/force", "/input/trigger/curl_meta", "/input/trigger/slide_meta", "/input/thumb_meta/proximity_meta" })
		CHECK(plus.IsInputPathValid(std::string("/user/hand/left") + path));

	// Everything the Touch has, they have
	for (const std::string& path : touch.GetValidInputPaths()) {
		CHECK(pro.IsInputPathValid(path));
		CHECK(plus.IsInputPathValid(path));
	}

	// The forces are raw inputs for the binding evaluator; the curl isn't one of SteamVR's
	auto raw = pro.RawComponents();
	CHECK(std::find(raw.begin(), raw.end(), std::make_pair(std::string("/user/hand/left/input/thumbrest/force"), std::string("/user/hand/left/input/thumbrest/force"))) != raw.end());
	CHECK(std::none_of(raw.begin(), raw.end(), [](const auto& r) { return r.first.find("curl") != std::string::npos; }));

	using enum ITrackedDevice::TrackedDeviceType;
	CHECK(pro.GetProperty<std::string>(vr::Prop_ModelNumber_String, HAND_LEFT) == "Meta Quest Pro (Left Controller)");
	CHECK(touch.GetProperty<std::string>(vr::Prop_ModelNumber_String, HAND_RIGHT) == "Oculus Quest (Right Controller)");
}

TEST_CASE("Odyssey_and_hand_profiles")
{
	HolographicInteractionProfile wmr, odyssey(HolographicInteractionProfile::Model::Odyssey);
	CHECK(odyssey.GetPath() == "/interaction_profiles/samsung/odyssey_controller");
	CHECK(std::string(*odyssey.GetOpenVRName()) == "holographic_controller");
	CHECK(odyssey.GetValidInputPaths() == wmr.GetValidInputPaths());

	// The hands: a pinch and a grasp, read as the trigger and the grip, converted from another controller's bindings
	MsftHandInteractionProfile hands;
	CHECK_FALSE(hands.GetOpenVRName().has_value());
	CHECK(hands.BindingConversionType() == "hand_interaction");
	// Its skeleton is estimated from the pinch and grasp, as the controllers' are
	CHECK(hands.GetOpenVRTrackinglevel() == vr::VRSkeletalTracking_Estimated);
	CHECK(hands.IsInputPathValid("/user/hand/left/input/select/value"));
	CHECK(hands.IsInputPathValid("/user/hand/right/input/squeeze/value"));
	CHECK_FALSE(hands.IsInputPathValid("/user/hand/right/output/haptic"));
	auto raw = hands.RawComponents();
	CHECK(std::find(raw.begin(), raw.end(), std::make_pair(std::string("/user/hand/right/input/select/value"), std::string("/user/hand/right/input/trigger/value"))) != raw.end());

	BindingFile index = ParseBindingFile(R"({ "controller_type": "knuckles", "bindings": { "/actions/main": { "sources": [
		{ "path": "/user/hand/right/input/trigger", "mode": "trigger", "inputs": { "pull": { "output": "/actions/main/in/fire" } } },
		{ "path": "/user/hand/right/input/a", "mode": "button", "inputs": { "click": { "output": "/actions/main/in/jump" } } } ] } } })");
	BindingFile converted = ConvertBindingFile(index, hands.BindingConversionType());
	const auto& sources = converted.sets.at("/actions/main").sources;
	CHECK(std::any_of(sources.begin(), sources.end(), [](const SourceBinding& b) { return b.path == "/user/hand/right/input/trigger"; }));
	CHECK(std::none_of(sources.begin(), sources.end(), [](const SourceBinding& b) { return b.path == "/user/hand/right/input/a"; }));

	// The simple controller keeps its own
	KhrSimpleInteractionProfile simple;
	CHECK(simple.BindingConversionType() == "khr_simple_controller");
}

TEST_CASE("Pico_g3_profile_has_its_own_few_inputs")
{
	PicoInteractionProfile g3(PicoInteractionProfile::Model::G3);
	CHECK(g3.GetPath() == "/interaction_profiles/bytedance/pico_g3_controller");
	CHECK(std::string(*g3.GetOpenVRName()) == "pico_controller");
	CHECK(g3.IsInputPathValid("/user/hand/right/input/trigger/click"));
	CHECK(g3.IsInputPathValid("/user/hand/left/input/menu/click"));
	CHECK(g3.IsInputPathValid("/user/hand/left/input/thumbstick/click"));
	CHECK(g3.IsInputPathValid("/user/hand/right/input/thumbstick/x"));
	CHECK(g3.IsInputPathValid("/user/hand/right/input/thumbstick/y"));
	CHECK_FALSE(g3.IsInputPathValid("/user/hand/right/input/thumbstick/touch"));
	CHECK_FALSE(g3.IsInputPathValid("/user/hand/right/input/a/click"));
	CHECK_FALSE(g3.IsInputPathValid("/user/hand/right/input/squeeze/value"));
	CHECK_FALSE(g3.IsInputPathValid("/user/hand/right/output/haptic"));
}

TEST_CASE("Hand_interaction_isnt_offered_with_a_controller_in_hand")
{
	CHECK(ExtHandInteractionProfile::ControllerInHands(std::string("Valve Knuckles Right"), std::nullopt));
	CHECK(ExtHandInteractionProfile::ControllerInHands(std::nullopt, std::string("Oculus Touch Left")));
	CHECK_FALSE(ExtHandInteractionProfile::ControllerInHands(std::nullopt, std::nullopt));
	CHECK_FALSE(ExtHandInteractionProfile::ControllerInHands(std::string(), std::nullopt)); // a device without a name
	CHECK_FALSE(ExtHandInteractionProfile::ControllerInHands(std::string("Pimax Hand Tracking Left"), std::string("Hand-Tracking Right")));
	CHECK(ExtHandInteractionProfile::ControllerInHands(std::string("Hand Tracking Left"), std::string("Valve Knuckles Right")));
}
