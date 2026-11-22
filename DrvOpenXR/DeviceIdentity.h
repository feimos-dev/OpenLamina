#pragma once

// What each tracked device says it is: the property answers that name it, from one table instead of fixed values
// spread over the device classes. An HMD takes the row of its OpenXR system's name; hardware without a row reports what
// the runtime says it is (its system name, and libmonado's device name and serial), and a missing row is no error.

#include "../OpenOVR/Misc/Input/InteractionProfile.h"

#include <cstdint>
#include <optional>
#include <string>

struct DeviceIdentity {
	// An empty string isn't answered by the table: the interaction profile's property, or the generic answer, is
	std::string trackingSystem, manufacturer, model, modelNumber, renderModel, controllerType, registeredDeviceType,
	    inputProfilePath, serial;
	std::string resourceRoot;
	uint64_t supportedButtons = 0;
	vr::ETrackedControllerRole role = vr::TrackedControllerRole_Invalid;
};

/** A device as libmonado names it; an empty string is one it doesn't give */
struct RuntimeDevice {
	std::string name, serial;
};

/** The HMD's manufacturer for an OpenXR system's vendor ID (XrSystemProperties::vendorId): the maker's name as
 * SteamVR gives it for the vendors known, else the first word of the runtime's name; empty without either */
std::string HmdManufacturer(uint32_t vendorId, const std::string& runtimeName);

/** The HMD of the OpenXR system of that name: its row if the table has one, else what the runtime says (the
 * manufacturer from the system's vendor ID, else the runtime's name; the system name as the model number, the
 * device's name and serial where libmonado gives them) on the generic answers that games branch on (tracking system,
 * controller type, render model). With no system name (no runtime yet) the generic answers alone. */
DeviceIdentity HmdIdentity(const std::string& xrSystemName, const RuntimeDevice& device = {}, uint32_t vendorId = 0,
    const std::string& runtimeName = std::string());

/** A controller of that profile in that hand: the profile's OpenVR name and render models, its buttons. The serial is
 * the runtime's where libmonado gives one, else one that is the hand's own. */
DeviceIdentity ControllerIdentity(const InteractionProfile& profile, vr::ETrackedControllerRole role, const std::string& runtimeSerial = std::string());

/** The type of an axis (Prop_Axis0Type_Int32 + axis) for a profile's OpenVR name that OpenComposite has a legacy
 * binding for (knuckles, oculus_touch, vive_controller); nothing for another */
std::optional<vr::EVRControllerAxisType> AxisTypeFor(const std::string& openVrName, int axis);

/** The interaction profile a controller of that name (libmonado's) most likely has, to show it before the runtime gives
 * the profile (a game's first frames): Index, Vive, Cosmos, Focus 3, Touch and Windows Mixed Reality controllers by
 * name, a simple controller for another. Null for no name, or a hand (hand tracking, or a controller made from it). */
const char* EarlyControllerProfilePath(const std::string& runtimeDeviceName);

/** A tracker (XR_MNDX_xdev_space): the generic tracker answers, with its serial */
DeviceIdentity TrackerIdentity(const std::string& xdevName, const std::string& serial);

/** The SteamVR driver folder a device's resources are in (Prop_ResourceRoot_String): its row's, else the driver in
 * braces at the start of its input profile path or render model ("{indexcontroller}/..."); empty when none says */
std::string ResourceRootOf(const DeviceIdentity& id);

/** EVRButtonId bits from the profile's valid input paths, for one hand (or both, for TrackedControllerRole_Invalid).
 * The system button is always there: the OpenXR runtime keeps it, as SteamVR's drivers report it. */
uint64_t SupportedButtonsFor(const InteractionProfile& profile, vr::ETrackedControllerRole role = vr::TrackedControllerRole_Invalid);
