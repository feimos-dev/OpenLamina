// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

#include "../DrvOpenXR/DeviceIdentity.h"
#include "Misc/Input/IndexControllerInteractionProfile.h"
#include "Misc/Input/KhrSimpleInteractionProfile.h"
#include "Misc/Input/OculusInteractionProfile.h"
#include "Misc/Input/ViveInteractionProfile.h"
#include "Misc/Input/ViveTrackerInteractionProfile.h"
#include "Misc/SoftFail.h"
#include "XrController.h"
#include "XrGenericTracker.h"

TEST_CASE("Hmd_default_row_matches_todays_answers")
{
	// The default row: an Oculus HMD's answers
	// A named string: gcc 13's -Wdangling-reference takes a reference returned from a temporary argument for a dangling one
	const std::string noName;
	const DeviceIdentity& hmd = HmdIdentity(noName);
	CHECK(hmd.trackingSystem == "oculus");
	CHECK(hmd.manufacturer == "Oculus");
	CHECK(hmd.renderModel == "oculusHmdRenderModel");
	CHECK(hmd.controllerType == "oculus"); // VRChat ignores an HMD without one
	CHECK(hmd.registeredDeviceType == "oculus/F00BAAF00F");
	CHECK(hmd.serial == "<unknown>");
	CHECK(hmd.model.empty()); // none answered: the interaction profile's, if any
	CHECK(hmd.modelNumber.empty());
	CHECK(hmd.inputProfilePath.empty());
	CHECK(hmd.supportedButtons == 0);
	CHECK(hmd.role == vr::TrackedControllerRole_Invalid);
}

TEST_CASE("Early_controller_profile_from_the_runtime_device_name")
{
	auto path = [](const char* name) {
		const char* p = EarlyControllerProfilePath(name);
		return std::string(p ? p : "");
	};
	// Monado's names for them (SteamVR's driver, Monado's own drivers)
	CHECK(path("Valve Knuckles Left") == "/interaction_profiles/valve/index_controller");
	CHECK(path("Valve Index Right Controller (vive)") == "/interaction_profiles/valve/index_controller");
	CHECK(path("HTC Vive Wand Left Controller (vive)") == "/interaction_profiles/htc/vive_controller");
	CHECK(path("Vive Cosmos Right Controller") == "/interaction_profiles/htc/vive_cosmos_controller");
	CHECK(path("Oculus Touch (Left)") == "/interaction_profiles/oculus/touch_controller");
	CHECK(path("Oculus Rift S Right Touch Controller") == "/interaction_profiles/oculus/touch_controller");
	CHECK(path("HP Reverb G2 Left Controller") == "/interaction_profiles/hp/mixed_reality_controller");
	CHECK(path("WMR Right Controller") == "/interaction_profiles/microsoft/motion_controller");
	// Another controller: a simple one, which every game's input has
	CHECK(path("PS Move Controller") == "/interaction_profiles/khr/simple_controller");
	CHECK(path("Fake Right") == "/interaction_profiles/khr/simple_controller");
	// A hand isn't a controller until hand tracking tracks it; no name, nothing to go by
	CHECK(path("Hand Tracking Left") == "");
	CHECK(path("Left Camera based Hand Tracker Hand") == "");
	CHECK(path("") == "");
}

TEST_CASE("Unknown_system_reports_itself")
{
	DeviceIdentity hmd = HmdIdentity("A headset nobody has heard of", {}, 0x1234, "Some Runtime 1.0");
	CHECK(hmd.modelNumber == "A headset nobody has heard of");
	// The maker isn't the model's name: the vendor's, else the runtime's
	CHECK(hmd.manufacturer == "Some");
	CHECK(HmdIdentity("Meta Quest 3", {}, 0x2833, "Oculus").manufacturer == "Oculus");
	CHECK(HmdIdentity("Valve Index", {}, 0x28de, "SteamVR/OpenXR").manufacturer == "Valve");
	CHECK(HmdIdentity("Vive", {}, 0x0bb4, "").manufacturer == "HTC");
	CHECK(HmdManufacturer(0x045e, "Windows Mixed Reality Runtime") == "WindowsMR");
	CHECK(HmdManufacturer(42, "Monado(XRT) by Collabora et al 'v21.0.0'") == "Monado");
	CHECK(HmdManufacturer(0, "").empty());
	CHECK(HmdManufacturer(1, "SteamVR/OpenXR") == "SteamVR");
	hmd = HmdIdentity("A headset nobody has heard of");
	CHECK(hmd.manufacturer == "Oculus"); // nothing to go by: the generic answer
	CHECK(hmd.serial == "A headset nobody has heard of-head");
	// What games branch on stays as it was
	CHECK(hmd.trackingSystem == "oculus");
	CHECK(hmd.controllerType == "oculus");

	// libmonado's device name and serial, where it gives them
	DeviceIdentity monado = HmdIdentity("Monado: Fake", { "Fake HMD", "SN0" }, 42, "Monado(XRT) by Collabora et al");
	CHECK(monado.modelNumber == "Fake HMD");
	CHECK(monado.serial == "SN0");
	CHECK(monado.manufacturer == "Monado");
}

TEST_CASE("Controllers_and_headset_have_different_serials")
{
	OculusTouchInteractionProfile touch;
	DeviceIdentity left = ControllerIdentity(touch, vr::TrackedControllerRole_LeftHand);
	DeviceIdentity right = ControllerIdentity(touch, vr::TrackedControllerRole_RightHand);
	DeviceIdentity hmd = HmdIdentity("Some system");
	CHECK(left.serial != right.serial);
	CHECK(left.serial != hmd.serial);
	CHECK(right.serial != hmd.serial);
	CHECK(ControllerIdentity(touch, vr::TrackedControllerRole_LeftHand, "SN1").serial == "SN1");
	CHECK(TrackerIdentity("Some tracker", "").serial != "");
	CHECK(TrackerIdentity("Some tracker", "T1").serial == "T1");
}

TEST_CASE("Trackers_name_the_tracking_system_their_profile_does")
{
	ViveTrackerInteractionProfile tracker;
	CHECK(TrackerIdentity("Some tracker", "").trackingSystem
	    == tracker.GetProperty<std::string>(vr::Prop_TrackingSystemName_String, ITrackedDevice::HAND_LEFT).value());
}

TEST_CASE("Axis_types_follow_the_controller")
{
	using vr::k_eControllerAxis_Joystick;
	using vr::k_eControllerAxis_None;
	using vr::k_eControllerAxis_TrackPad;
	using vr::k_eControllerAxis_Trigger;
	CHECK(AxisTypeFor("knuckles", 0) == k_eControllerAxis_TrackPad);
	CHECK(AxisTypeFor("knuckles", 1) == k_eControllerAxis_Trigger);
	CHECK(AxisTypeFor("vive_controller", 0) == k_eControllerAxis_TrackPad);
	CHECK(AxisTypeFor("vive_controller", 1) == k_eControllerAxis_Trigger);
	// A Touch controller: stick, trigger, grip
	CHECK(AxisTypeFor("oculus_touch", 0) == k_eControllerAxis_Joystick);
	CHECK(AxisTypeFor("oculus_touch", 1) == k_eControllerAxis_Trigger);
	CHECK(AxisTypeFor("oculus_touch", 2) == k_eControllerAxis_Trigger);
	CHECK(AxisTypeFor("oculus_touch", 3) == k_eControllerAxis_None);
	CHECK_FALSE(AxisTypeFor("hpmotioncontroller", 0).has_value());
	CHECK_FALSE(AxisTypeFor("knuckles", 5).has_value());
}

TEST_CASE("Index_controller_identity")
{
	IndexControllerInteractionProfile index;
	DeviceIdentity right = ControllerIdentity(index, vr::TrackedControllerRole_RightHand);
	CHECK(right.controllerType == "knuckles");
	CHECK(right.renderModel == "{indexcontroller}valve_controller_knu_1_0_right");
	// As SteamVR's Lighthouse driver answers: its prefix and the serial; the Index driver's input profile
	CHECK(right.registeredDeviceType == "valve/index_controller" + right.serial);
	CHECK(right.inputProfilePath == "{indexcontroller}/input/index_controller_profile.json");
	CHECK(ControllerIdentity(index, vr::TrackedControllerRole_RightHand, "LHR-1F0DBAC0").registeredDeviceType
	    == "valve/index_controllerLHR-1F0DBAC0");
	// Without the runtime's serial, the one made of the hand
	CHECK(right.registeredDeviceType == "valve/index_controllerknuckles-right");
	CHECK(right.role == vr::TrackedControllerRole_RightHand);
	CHECK((right.supportedButtons & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger)) != 0);
	CHECK((right.supportedButtons & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad)) != 0); // the trackpad and stick are Axis0

	DeviceIdentity left = ControllerIdentity(index, vr::TrackedControllerRole_LeftHand);
	CHECK(left.renderModel == "{indexcontroller}valve_controller_knu_1_0_left");
	CHECK(left.registeredDeviceType == "valve/index_controller" + left.serial);
	CHECK(left.inputProfilePath == "{indexcontroller}/input/index_controller_profile.json");
}

TEST_CASE("Controllers_have_their_SteamVR_driver_input_profile")
{
	ViveWandInteractionProfile vive;
	DeviceIdentity wand = ControllerIdentity(vive, vr::TrackedControllerRole_LeftHand, "LHR-0000");
	CHECK(wand.inputProfilePath == "{htc}/input/vive_controller_profile.json");
	CHECK(wand.registeredDeviceType == "oculus/F00BAAF00F_Controller_Left");

	// The Oculus driver's device type stays, with its Touch input profile
	OculusTouchInteractionProfile touch;
	DeviceIdentity right = ControllerIdentity(touch, vr::TrackedControllerRole_RightHand);
	CHECK(right.inputProfilePath == "{oculus}/input/touch_profile.json");
	CHECK(right.registeredDeviceType == "oculus/F00BAAF00F_Controller_Right");

	// A controller SteamVR has no driver for: none, so the tracking system's name is answered
	KhrSimpleInteractionProfile simple;
	CHECK(ControllerIdentity(simple, vr::TrackedControllerRole_RightHand).inputProfilePath.empty());
}

TEST_CASE("Headset_maker_from_the_device_name")
{
	// libmonado's device name starts with a known maker: that one, not the runtime's name
	CHECK(HmdIdentity("Monado: Pimax Dream Air", { "Pimax Dream Air", "P5" }, 0, "Monado(XRT) by Collabora et al").manufacturer
	    == "Pimax");
	CHECK(HmdIdentity("Monado: Valve Index", { "Valve Index", "" }, 0, "Monado(XRT)").manufacturer == "Valve");
	// The vendor ID still comes first
	CHECK(HmdIdentity("x", { "Pimax Crystal", "" }, 0x28de, "Monado").manufacturer == "Valve");
	// An unknown first word: the runtime's name, as before
	CHECK(HmdIdentity("Monado: Simulated HMD", { "Simulated HMD", "" }, 0, "Monado(XRT)").manufacturer == "Monado");
}

TEST_CASE("Touch_buttons_include_A_and_B_on_right")
{
	OculusTouchInteractionProfile touch;
	uint64_t right = ControllerIdentity(touch, vr::TrackedControllerRole_RightHand).supportedButtons;
	CHECK((right & vr::ButtonMaskFromId(vr::k_EButton_A)) != 0);
	CHECK((right & vr::ButtonMaskFromId(vr::k_EButton_ApplicationMenu)) != 0); // B, as SteamVR's Touch driver has it
	CHECK((right & vr::ButtonMaskFromId(vr::k_EButton_Grip)) != 0);
	CHECK((right & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger)) != 0);

	// The left hand has X and Y (A and ApplicationMenu too) and the menu button
	uint64_t left = ControllerIdentity(touch, vr::TrackedControllerRole_LeftHand).supportedButtons;
	CHECK((left & vr::ButtonMaskFromId(vr::k_EButton_A)) != 0);
	CHECK((left & vr::ButtonMaskFromId(vr::k_EButton_ApplicationMenu)) != 0);
	CHECK(ControllerIdentity(touch, vr::TrackedControllerRole_LeftHand).controllerType == "oculus_touch");
}

TEST_CASE("Simple_controller_select_is_the_trigger")
{
	KhrSimpleInteractionProfile simple;
	uint64_t right = ControllerIdentity(simple, vr::TrackedControllerRole_RightHand).supportedButtons;
	CHECK((right & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger)) != 0);
	CHECK((right & vr::ButtonMaskFromId(vr::k_EButton_ApplicationMenu)) != 0);
}

TEST_CASE("Controller_properties_by_hand")
{
	IndexControllerInteractionProfile index;
	XrController left(XrController::XCT_LEFT, index);
	XrController right(XrController::XCT_RIGHT, index);
	vr::ETrackedPropertyError error = vr::TrackedProp_UnknownProperty;

	CHECK(left.GetInt32TrackedDeviceProperty(vr::Prop_DeviceClass_Int32, &error) == vr::TrackedDeviceClass_Controller);
	CHECK(error == vr::TrackedProp_Success);
	error = vr::TrackedProp_UnknownProperty;
	CHECK(left.GetInt32TrackedDeviceProperty(vr::Prop_ControllerRoleHint_Int32, &error) == vr::TrackedControllerRole_LeftHand);
	CHECK(error == vr::TrackedProp_Success);
	CHECK(right.GetInt32TrackedDeviceProperty(vr::Prop_ControllerRoleHint_Int32, &error) == vr::TrackedControllerRole_RightHand);

	// An Index controller has a trackpad, and answers its own controller type
	CHECK(right.GetInt32TrackedDeviceProperty(vr::Prop_Axis0Type_Int32, &error) == vr::k_eControllerAxis_TrackPad);
	char type[64] = {};
	right.GetStringTrackedDeviceProperty(vr::Prop_ControllerType_String, type, sizeof(type), &error);
	CHECK(std::string(type) == "knuckles");

	char l[64] = {}, r[64] = {};
	left.GetStringTrackedDeviceProperty(vr::Prop_SerialNumber_String, l, sizeof(l), &error);
	right.GetStringTrackedDeviceProperty(vr::Prop_SerialNumber_String, r, sizeof(r), &error);
	CHECK(std::string(l) != std::string(r));

	CHECK(left.GetBoolTrackedDeviceProperty(vr::Prop_HasControllerComponent_Bool, &error));

	// No battery data: no battery status claimed
	error = vr::TrackedProp_UnknownProperty;
	CHECK_FALSE(left.GetBoolTrackedDeviceProperty(vr::Prop_DeviceProvidesBatteryStatus_Bool, &error));
	CHECK(error == vr::TrackedProp_Success);
}

TEST_CASE("SteamVR_server_answers_for_every_device")
{
	// What SteamVR's server answers for every device whatever its driver, here for a controller of a profile that sets
	// none of them
	KhrSimpleInteractionProfile simple;
	XrController left(XrController::XCT_LEFT, simple);
	vr::ETrackedPropertyError error = vr::TrackedProp_UnknownProperty;
	CHECK(left.GetBoolTrackedDeviceProperty(vr::Prop_ForceSystemLayerUseAppPoses_Bool, &error));
	CHECK(error == vr::TrackedProp_Success);
	CHECK(left.GetBoolTrackedDeviceProperty(vr::Prop_Hmd_AllowsClientToControlTextureIndex, &error));
	for (vr::ETrackedDeviceProperty prop : { vr::Prop_HasCameraComponent_Bool, vr::Prop_HasDriverDirectModeComponent_Bool,
	         vr::Prop_HasVirtualDisplayComponent_Bool }) {
		error = vr::TrackedProp_UnknownProperty;
		CHECK_FALSE(left.GetBoolTrackedDeviceProperty(prop, &error));
		CHECK(error == vr::TrackedProp_Success);
	}
	error = vr::TrackedProp_UnknownProperty;
	CHECK(left.GetInt32TrackedDeviceProperty(vr::Prop_ControllerHandSelectionPriority_Int32, &error) == 0);
	CHECK(error == vr::TrackedProp_Success);
	// Still unknown: a property SteamVR's server doesn't answer for every device
	CHECK_FALSE(left.GetBoolTrackedDeviceProperty(vr::Prop_DeviceIsWireless_Bool, &error));
	CHECK(error == vr::TrackedProp_UnknownProperty);
}

TEST_CASE("Index_controller_answers_as_SteamVR_lighthouse_driver")
{
	IndexControllerInteractionProfile index;
	XrController left(XrController::XCT_LEFT, index);
	XrController right(XrController::XCT_RIGHT, index);
	vr::ETrackedPropertyError error = vr::TrackedProp_UnknownProperty;
	CHECK(left.GetBoolTrackedDeviceProperty(vr::Prop_DeviceIsWireless_Bool, &error));
	CHECK(error == vr::TrackedProp_Success);
	CHECK(left.GetBoolTrackedDeviceProperty(vr::Prop_DeviceCanPowerOff_Bool, &error));
	CHECK(left.GetBoolTrackedDeviceProperty(vr::Prop_Identifiable_Bool, &error));
	CHECK_FALSE(left.GetBoolTrackedDeviceProperty(vr::Prop_Firmware_UpdateAvailable_Bool, &error));
	CHECK(error == vr::TrackedProp_Success);
	CHECK(left.GetFloatTrackedDeviceProperty(vr::Prop_DisplayMaxAnalogGain_Float, &error) == 1.0f);
	char l[128] = {}, r[128] = {};
	left.GetStringTrackedDeviceProperty(vr::Prop_NamedIconPathDeviceReady_String, l, sizeof(l), &error);
	right.GetStringTrackedDeviceProperty(vr::Prop_NamedIconPathDeviceReady_String, r, sizeof(r), &error);
	CHECK(std::string(l) == "{indexcontroller}/icons/left_controller_status_ready.png");
	CHECK(std::string(r) == "{indexcontroller}/icons/right_controller_status_ready.png");
	// Its own answer stays: an Index controller has a controller component
	CHECK(left.GetBoolTrackedDeviceProperty(vr::Prop_HasControllerComponent_Bool, &error));
}

TEST_CASE("Resource_root_is_the_driver_folder")
{
	// As SteamVR's drivers answer it: the folder their resources are in, named in their input profile's path
	IndexControllerInteractionProfile index;
	ViveWandInteractionProfile wand;
	OculusTouchInteractionProfile touch;
	CHECK(ResourceRootOf(ControllerIdentity(index, vr::TrackedControllerRole_RightHand)) == "indexcontroller");
	CHECK(ResourceRootOf(ControllerIdentity(wand, vr::TrackedControllerRole_LeftHand)) == "htc");
	CHECK(ResourceRootOf(ControllerIdentity(touch, vr::TrackedControllerRole_LeftHand)) == "oculus");
	CHECK(ResourceRootOf(TrackerIdentity("Some tracker", "")) == "htc");
	// The generic headset's row: the Oculus driver's
	const std::string noName;
	CHECK(ResourceRootOf(HmdIdentity(noName)) == "oculus");
	DeviceIdentity braces;
	braces.renderModel = "{somedriver}some_model";
	CHECK(ResourceRootOf(braces) == "somedriver");
	braces.renderModel = "{}empty";
	CHECK(ResourceRootOf(braces).empty());
	braces.renderModel = "{unclosed";
	CHECK(ResourceRootOf(braces).empty());
}

TEST_CASE("Resource_root_and_actual_tracking_system_answered")
{
	auto str = [](ITrackedDevice& device, vr::ETrackedDeviceProperty prop, vr::ETrackedPropertyError* error) {
		char value[64] = {};
		device.GetStringTrackedDeviceProperty(prop, value, sizeof(value), error);
		return std::string(value);
	};
	IndexControllerInteractionProfile index;
	XrController right(XrController::XCT_RIGHT, index);
	vr::ETrackedPropertyError error = vr::TrackedProp_UnknownProperty;
	CHECK(str(right, vr::Prop_ResourceRoot_String, &error) == "indexcontroller");
	CHECK(error == vr::TrackedProp_Success);
	error = vr::TrackedProp_UnknownProperty;
	// The profile's tracking system, as Prop_TrackingSystemName_String gives it
	CHECK(str(right, vr::Prop_ActualTrackingSystemName_String, &error) == "lighthouse");
	CHECK(error == vr::TrackedProp_Success);

	// A controller whose SteamVR driver folder isn't known: not answered; its tracking system still is
	KhrSimpleInteractionProfile simple;
	XrController left(XrController::XCT_LEFT, simple);
	const std::string tracking = str(left, vr::Prop_TrackingSystemName_String, &error);
	CHECK_FALSE(tracking.empty());
	CHECK(str(left, vr::Prop_ResourceRoot_String, &error).empty());
	CHECK(error == vr::TrackedProp_UnknownProperty);
	CHECK(str(left, vr::Prop_ActualTrackingSystemName_String, &error) == tracking);

	XrXDevPropertiesMNDX properties = {};
	XrGenericTracker tracker(simple, properties, 0, XR_NULL_HANDLE);
	CHECK(str(tracker, vr::Prop_ResourceRoot_String, &error) == "htc");
	CHECK(str(tracker, vr::Prop_ActualTrackingSystemName_String, &error) == "lighthouse");

	// Too small a buffer: the size needed, as for every string property
	char tiny[3] = {};
	CHECK(right.GetStringTrackedDeviceProperty(vr::Prop_ResourceRoot_String, tiny, sizeof(tiny), &error) == 16);
	CHECK(error == vr::TrackedProp_BufferTooSmall);
}

TEST_CASE("Each_unknown_property_is_logged_once")
{
	OculusTouchInteractionProfile touch;
	XrController left(XrController::XCT_LEFT, touch);
	vr::ETrackedPropertyError error;
	size_t before = oc_soft_sites().size();
	left.GetFloatTrackedDeviceProperty((vr::ETrackedDeviceProperty)91001, &error);
	left.GetFloatTrackedDeviceProperty((vr::ETrackedDeviceProperty)91002, &error);
	left.GetFloatTrackedDeviceProperty((vr::ETrackedDeviceProperty)91001, &error);
	CHECK(error == vr::TrackedProp_UnknownProperty);
	CHECK(oc_soft_sites().size() == before + 2);
}

TEST_CASE("A tracker without a space is connected and has no pose")
{
	XrXDevPropertiesMNDX properties = {};
	KhrSimpleInteractionProfile profile;
	XrGenericTracker tracker(profile, properties, 0, XR_NULL_HANDLE);
	vr::TrackedDevicePose_t pose = {};
	pose.bPoseIsValid = true;
	tracker.GetPose(vr::TrackingUniverseStanding, &pose, TrackingStateType_Rendering);
	CHECK(pose.bDeviceIsConnected);
	CHECK_FALSE(pose.bPoseIsValid);
	CHECK(pose.eTrackingResult == vr::TrackingResult_Running_OutOfRange);
}

TEST_CASE("A tracker whose space went with its session isn't located in it")
{
	// A handle no runtime gave: locating or destroying it would be a call on a session that's gone
	XrXDevPropertiesMNDX properties = {};
	KhrSimpleInteractionProfile profile;
	XrGenericTracker tracker(profile, properties, 0, (XrSpace)(uintptr_t)0x1234);
	tracker.ForgetSpace();
	vr::TrackedDevicePose_t pose = {};
	pose.bPoseIsValid = true;
	tracker.GetPose(vr::TrackingUniverseStanding, &pose, TrackingStateType_Rendering);
	CHECK(pose.bDeviceIsConnected);
	CHECK_FALSE(pose.bPoseIsValid);
}
