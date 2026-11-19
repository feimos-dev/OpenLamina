// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

#include "../DrvOpenXR/DeviceIdentity.h"
#include "Misc/Input/IndexControllerInteractionProfile.h"
#include "Misc/Input/KhrSimpleInteractionProfile.h"
#include "Misc/Input/OculusInteractionProfile.h"
#include "Misc/SoftFail.h"
#include "XrController.h"

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

TEST_CASE("Unknown_system_reports_itself")
{
	DeviceIdentity hmd = HmdIdentity("A runtime nobody has heard of");
	CHECK(hmd.modelNumber == "A runtime nobody has heard of");
	CHECK(hmd.manufacturer == "A runtime nobody has heard of");
	CHECK(hmd.manufacturer != "Oculus");
	CHECK(hmd.serial == "A runtime nobody has heard of-head");
	// What games branch on stays as it was
	CHECK(hmd.trackingSystem == "oculus");
	CHECK(hmd.controllerType == "oculus");

	// libmonado's device name and serial, where it gives them
	DeviceIdentity monado = HmdIdentity("Monado: Fake", { "Fake HMD", "SN0" });
	CHECK(monado.modelNumber == "Fake HMD");
	CHECK(monado.serial == "SN0");
	CHECK(monado.manufacturer == "Monado: Fake");
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
	CHECK(right.registeredDeviceType == "oculus/F00BAAF00F_Controller_Right");
	CHECK(right.role == vr::TrackedControllerRole_RightHand);
	CHECK((right.supportedButtons & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger)) != 0);
	CHECK((right.supportedButtons & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad)) != 0); // the trackpad and stick are Axis0

	DeviceIdentity left = ControllerIdentity(index, vr::TrackedControllerRole_LeftHand);
	CHECK(left.renderModel == "{indexcontroller}valve_controller_knu_1_0_left");
	CHECK(left.registeredDeviceType == "oculus/F00BAAF00F_Controller_Left");
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
