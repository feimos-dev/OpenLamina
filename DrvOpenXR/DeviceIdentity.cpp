#include "stdafx.h"

#include "DeviceIdentity.h"

#include "../OpenOVR/Misc/Input/Bindings/LegacyBindings.h"

#include <array>
#include <map>
#include <mutex>

namespace {

DeviceIdentity DefaultHmd()
{
	// An Oculus HMD's answers, as SteamVR's Oculus driver gives them
	DeviceIdentity hmd;
	hmd.trackingSystem = "oculus";
	hmd.manufacturer = "Oculus";
	hmd.renderModel = "oculusHmdRenderModel";
	hmd.controllerType = "oculus"; // VRChat ignores an HMD without one
	hmd.registeredDeviceType = "oculus/F00BAAF00F";
	hmd.serial = "<unknown>";
	return hmd;
}

// The rows by OpenXR system name: the strings SteamVR gives that hardware. None yet.
const std::map<std::string, DeviceIdentity>& HmdRows()
{
	static const std::map<std::string, DeviceIdentity> rows = {};
	return rows;
}

// The button an input path's component gives (the part after /input/), as SteamVR's drivers map them
uint64_t ButtonsForComponent(const std::string& component)
{
	using vr::ButtonMaskFromId;
	if (component == "system")
		return ButtonMaskFromId(vr::k_EButton_System);
	if (component == "menu" || component == "b" || component == "y")
		return ButtonMaskFromId(vr::k_EButton_ApplicationMenu); // the Touch driver's B and Y
	if (component == "a" || component == "x")
		return ButtonMaskFromId(vr::k_EButton_A);
	if (component == "squeeze")
		return ButtonMaskFromId(vr::k_EButton_Grip) | ButtonMaskFromId(vr::k_EButton_Axis2);
	if (component == "trigger" || component == "select") // the simple controller's select is its trigger
		return ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger);
	if (component == "thumbstick" || component == "trackpad") {
		// Axis0, and the d-pad the legacy input reads from it
		return ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad) | ButtonMaskFromId(vr::k_EButton_DPad_Left)
		    | ButtonMaskFromId(vr::k_EButton_DPad_Up) | ButtonMaskFromId(vr::k_EButton_DPad_Right)
		    | ButtonMaskFromId(vr::k_EButton_DPad_Down);
	}
	return 0;
}

} // namespace

DeviceIdentity HmdIdentity(const std::string& xrSystemName, const RuntimeDevice& device)
{
	auto it = HmdRows().find(xrSystemName);
	if (it != HmdRows().end())
		return it->second;

	DeviceIdentity id = DefaultHmd();
	if (xrSystemName.empty())
		return id;
	// The runtime's own words: the system name is all OpenXR says of who made it
	id.manufacturer = xrSystemName;
	id.modelNumber = device.name.empty() ? xrSystemName : device.name;
	id.serial = device.serial.empty() ? xrSystemName + "-head" : device.serial;
	return id;
}

uint64_t SupportedButtonsFor(const InteractionProfile& profile, vr::ETrackedControllerRole role)
{
	uint64_t buttons = vr::ButtonMaskFromId(vr::k_EButton_System);
	std::string hand = role == vr::TrackedControllerRole_LeftHand ? "/user/hand/left/input/"
	    : role == vr::TrackedControllerRole_RightHand            ? "/user/hand/right/input/"
	                                                             : "";
	for (const std::string& path : profile.GetValidInputPaths()) {
		size_t input = path.find("/input/");
		if (input == std::string::npos || (!hand.empty() && path.compare(0, hand.size(), hand) != 0))
			continue;
		size_t start = input + 7, end = path.find('/', start);
		buttons |= ButtonsForComponent(path.substr(start, end == std::string::npos ? std::string::npos : end - start));
	}
	return buttons;
}

DeviceIdentity ControllerIdentity(const InteractionProfile& profile, vr::ETrackedControllerRole role, const std::string& runtimeSerial)
{
	DeviceIdentity id;
	// The generic device answers (XrTrackedDevice::Identity's)
	id.trackingSystem = "oculus";
	id.manufacturer = "Oculus";
	id.role = role;
	if (std::optional<const char*> name = profile.GetOpenVRName())
		id.controllerType = *name;

	// Each device has a serial of its own (tools key devices by it): the runtime's, else one made of the hand
	const char* hand = role == vr::TrackedControllerRole_LeftHand ? "left" : role == vr::TrackedControllerRole_RightHand ? "right"
	                                                                                                                     : "tracked";
	id.serial = !runtimeSerial.empty() ? runtimeSerial : (id.controllerType.empty() ? std::string("controller") : id.controllerType) + "-" + hand;

	// Resonite tells controllers apart by their render model: the profile's, else OpenComposite's own
	bool left = role == vr::TrackedControllerRole_LeftHand;
	if (role == vr::TrackedControllerRole_LeftHand || role == vr::TrackedControllerRole_RightHand) {
		std::optional<const char*> model = left ? profile.GetLeftHandRenderModelName() : profile.GetRightHandRenderModelName();
		id.renderModel = model ? *model : (left ? "renderLeftHand" : "renderRightHand");
		id.registeredDeviceType = left ? "oculus/F00BAAF00F_Controller_Left" : "oculus/F00BAAF00F_Controller_Right";
	} else {
		id.renderModel = "renderObject0";
	}
	id.supportedButtons = SupportedButtonsFor(profile, role);
	return id;
}

DeviceIdentity TrackerIdentity(const std::string& xdevName, const std::string& serial)
{
	// The generic tracker answers (XrGenericTracker.cpp); the render model is the profile's OpenVR name, set by the tracker
	DeviceIdentity id;
	id.trackingSystem = "oculus";
	id.manufacturer = "Oculus";
	id.controllerType = "vive_tracker_handheld_object";
	id.modelNumber = "Vive Tracker Handheld Object";
	// The xdev's own serial, else one made of its name
	id.serial = !serial.empty() ? serial : "tracker-" + xdevName;
	id.role = vr::TrackedControllerRole_Invalid;
	return id;
}

std::optional<vr::EVRControllerAxisType> AxisTypeFor(const std::string& openVrName, int axis)
{
	if (axis < 0 || axis > 4)
		return std::nullopt;
	if (openVrName != "knuckles" && openVrName != "oculus_touch" && openVrName != "vive_controller")
		return std::nullopt;
	// The legacy binding OpenComposite has for the controller says what each axis is; both hands are the same here
	static std::mutex lock;
	static std::map<std::string, std::array<vr::EVRControllerAxisType, 5>> cache;
	std::lock_guard<std::mutex> guard(lock);
	auto it = cache.find(openVrName);
	if (it == cache.end()) {
		BindingFile file = BuiltInLegacyBindings(openVrName);
		std::array<vr::EVRControllerAxisType, 5> types;
		for (int i = 0; i < 5; i++)
			types[i] = LegacyAxisType(file, "left", i);
		it = cache.emplace(openVrName, types).first;
	}
	return it->second[axis];
}
