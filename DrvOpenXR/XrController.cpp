#include "XrController.h"
#include "XrBackend.h"

#include "../OpenOVR/Misc/LibMonado.h"

// HACK: grab the pose from BaseInput
#include "../OpenOVR/Misc/xrmoreutils.h"
#include "../OpenOVR/Misc/Config.h"
#include "../OpenOVR/Misc/Input/Bindings/LegacyBindings.h"
#include "../OpenOVR/Reimpl/BaseInput.h"
#include "generated/static_bases.gen.h"

XrController::XrController(XrController::XrControllerType type, const InteractionProfile& profile)
    : type(type), profile(profile)
{
	InitialiseDevice(GetHand() + 1);
}

#define TRY_PROFILE_PROP(type)                                                \
	do {                                                                      \
		std::optional<type> ret = profile.GetProperty<type>(prop, GetHand()); \
		if (ret.has_value())                                                  \
			return *ret;                                                      \
	} while (0)

// properties
bool XrController::GetBoolTrackedDeviceProperty(vr::ETrackedDeviceProperty prop, vr::ETrackedPropertyError* pErrorL)
{
	if (pErrorL)
		*pErrorL = vr::TrackedProp_Success;

	TRY_PROFILE_PROP(bool);

	if (prop == vr::Prop_HasControllerComponent_Bool)
		return true;

	// The battery, from libmonado when Monado offers it (asked only for the battery's own properties)
	const char* role = type == XCT_LEFT ? "left" : "right";
	if (std::optional<bool> battery = BatteryBoolProperty(LibMonado::Get(), prop, role))
		return *battery;
	return XrTrackedDevice::GetBoolTrackedDeviceProperty(prop, pErrorL);
}

float XrController::GetFloatTrackedDeviceProperty(vr::ETrackedDeviceProperty prop, vr::ETrackedPropertyError* pErrorL)
{
	// Only the battery here; the rest as before
	if (prop == vr::Prop_DeviceBatteryPercentage_Float) {
		std::optional<LibMonado::Battery> battery = LibMonado::Get().BatteryForRole(type == XCT_LEFT ? "left" : "right");
		if (battery && battery->present) {
			if (pErrorL)
				*pErrorL = vr::TrackedProp_Success;
			return battery->charge;
		}
	}
	return XrTrackedDevice::GetFloatTrackedDeviceProperty(prop, pErrorL);
}

int32_t XrController::GetInt32TrackedDeviceProperty(vr::ETrackedDeviceProperty prop, vr::ETrackedPropertyError* pErrorL)
{
	if (pErrorL)
		*pErrorL = vr::TrackedProp_Success;

	TRY_PROFILE_PROP(int32_t);

	// The axis types SteamVR's legacy binding file for the controller gives it
	const bool isAxis = prop >= vr::Prop_Axis0Type_Int32 && prop <= vr::Prop_Axis4Type_Int32;
	if (isAxis && (type == XCT_LEFT || type == XCT_RIGHT)) {
		if (const BindingFile* legacy = LoadLegacyBindings(profile.GetOpenVRName().value_or("generic")))
			return LegacyAxisType(*legacy, type == XCT_LEFT ? "left" : "right", prop - vr::Prop_Axis0Type_Int32);
	}

	// The controllers OpenComposite has a legacy binding for have the axes SteamVR's give them (an Index or Vive
	// controller has a trackpad); the others are a Touch controller's
	if (isAxis) {
		if (std::optional<vr::EVRControllerAxisType> axisType = AxisTypeFor(profile.GetOpenVRName().value_or(""), prop - vr::Prop_Axis0Type_Int32))
			return *axisType;
		switch (prop) {
		case vr::Prop_Axis0Type_Int32:
			return vr::k_eControllerAxis_Joystick;

		case vr::Prop_Axis1Type_Int32:
		case vr::Prop_Axis2Type_Int32:
			return vr::k_eControllerAxis_Trigger;

		default:
			return vr::k_eControllerAxis_None;
		}
	}

	return XrTrackedDevice::GetInt32TrackedDeviceProperty(prop, pErrorL);
}

uint64_t XrController::GetUint64TrackedDeviceProperty(vr::ETrackedDeviceProperty prop, vr::ETrackedPropertyError* pErrorL)
{
	if (pErrorL)
		*pErrorL = vr::TrackedProp_Success;

	TRY_PROFILE_PROP(uint64_t);

	// The old input system's buttons: those SteamVR's legacy binding file for the controller gives it, else those the
	// controller's interaction profile has (DeviceIdentity)
	if (prop == vr::Prop_SupportedButtons_Uint64) {
		if (type == XCT_LEFT || type == XCT_RIGHT) {
			if (const BindingFile* legacy = LoadLegacyBindings(profile.GetOpenVRName().value_or("generic")))
				return LegacySupportedButtons(*legacy, type == XCT_LEFT ? "left" : "right");
		}
		return Identity().supportedButtons;
	}

	return XrTrackedDevice::GetUint64TrackedDeviceProperty(prop, pErrorL);
}

uint32_t XrController::GetStringTrackedDeviceProperty(vr::ETrackedDeviceProperty prop,
    char* value, uint32_t bufferSize, vr::ETrackedPropertyError* pErrorL)
{

	if (pErrorL)
		*pErrorL = vr::TrackedProp_Success;

	std::optional<std::string> ret = profile.GetProperty<std::string>(prop, GetHand());
	if (ret.has_value())
		return AnswerString(*ret, value, bufferSize, pErrorL);

	// The rest from the controller's row (Identity): Resonite tells controllers apart by the render model name
	return XrTrackedDevice::GetStringTrackedDeviceProperty(prop, value, bufferSize, pErrorL);
}

DeviceIdentity XrController::Identity()
{
	bool left = type == XCT_LEFT;
	std::optional<LibMonado::DeviceInfo> device = LibMonado::Get().DeviceInfoForRole(left ? "left" : "right");
	return ControllerIdentity(*GetInteractionProfile(), left ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand,
	    device ? device->serial : std::string());
}

ITrackedDevice::TrackedDeviceType XrController::GetHand()
{
	return type == XCT_LEFT ? HAND_LEFT : HAND_RIGHT;
}

void XrController::GetPose(vr::ETrackingUniverseOrigin origin, vr::TrackedDevicePose_t* pose, ETrackingStateType trackingState, int64_t xrTime)
{
	auto sessionLock = xr_session.lock_shared(); // held while reading: a session restart waits for it
	// Default to an invalid pose
	ZeroMemory(pose, sizeof(*pose));
	pose->bDeviceIsConnected = true;
	pose->bPoseIsValid = false;
	pose->eTrackingResult = vr::TrackingResult_Running_OutOfRange;

	// No session (it was lost): deactivated until it's made again
	if (XrBackend::CurrentSessionGone()) {
		pose->bDeviceIsConnected = false;
		pose->eTrackingResult = vr::TrackingResult_Uninitialized;
		return;
	}

	BaseInput* input = GetUnsafeBaseInput();
	if (input == nullptr)
		return;

	// Lie and say the pose is valid if the actions haven't even been loaded yet.
	// This is a workaround for games like DCS, which appear to require valid poses before
	// it will even attempt to request the controller state (and thus create the actions).
	if (!input->AreActionsLoaded()) {
		pose->bPoseIsValid = true;
		pose->eTrackingResult = vr::TrackingResult_Running_OK;
		pose->mDeviceToAbsoluteTracking = G2S_m34(glm::identity<glm::mat4>());
	}

	xrTime = PoseTime(trackingState, xrTime);
	XrSpace space = XR_NULL_HANDLE;

	// Specifically use grip pose, since that's what InteractionProfile::GetGripToSteamVRTransform uses
	GetBaseInput()->GetHandSpace(DeviceIndex(), space, false);

	if (!space)
		return;

	// Find the hand transform matrix, and include that
	glm::mat4 transform = profile.GetGripToSteamVRTransform(GetHand());

	if (xr_utils::PoseFromSpace(pose, space, origin, transform, xrTime)) {
		isPoseFromHandTracking = false;
		return;
	}

	// Fallback to pose from hand tracking if the controller one isn't valid
	GetPoseFromHandTracking(origin, input, pose, xrTime);
	isPoseFromHandTracking = true;
}

void XrController::GetPoseFromHandTracking(vr::ETrackingUniverseOrigin origin, BaseInput* input, vr::TrackedDevicePose_t* pose, int64_t xrTime)
{
	if (!xr_gbl || !xr_gbl->handTrackingProperties.supportsHandTracking)
		return;

	if (!input->handTrackers[(int)GetHand()])
		return;

	// In the space the game asked for, as the controller's own pose is
	const XrSpace baseSpace = xr_space_from_tracking_origin(origin);
	if (!baseSpace)
		return;

	XrHandJointsLocateInfoEXT locateInfo = { XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT };
	locateInfo.baseSpace = baseSpace;
	locateInfo.time = xrTime > 0 ? xrTime : xr_gbl->GetBestTime();

	XrHandJointVelocitiesEXT velocities = { XR_TYPE_HAND_JOINT_VELOCITIES_EXT };
	velocities.jointCount = XR_HAND_JOINT_COUNT_EXT;
	std::vector<XrHandJointVelocityEXT> jointVelocities(velocities.jointCount);
	velocities.jointVelocities = jointVelocities.data();

	XrHandJointLocationsEXT locations = { XR_TYPE_HAND_JOINT_LOCATIONS_EXT };
	locations.jointCount = XR_HAND_JOINT_COUNT_EXT;
	std::vector<XrHandJointLocationEXT> jointLocations(locations.jointCount);
	locations.jointLocations = jointLocations.data();
	locations.next = &velocities;

	// On failure the pose stays as it was (the controller's), so the hand doesn't jump
	if (XR_FAILED(xr_ext->xrLocateHandJointsEXT(input->handTrackers[(int)GetHand()], &locateInfo, &locations)))
		return;

	xr_utils::PoseFromHandTracking(pose, locations, velocities, GetHand() == HAND_RIGHT);
}

vr::ETrackedDeviceClass XrController::GetTrackedDeviceClass()
{
	return vr::TrackedDeviceClass_Controller;
}

bool XrController::IsPoseFromHandTracking()
{
	return isPoseFromHandTracking;
}

bool XrController::IsHandTrackingValid()
{
	return isHandTrackingValid;
}

void XrController::SetHandTrackingValid(bool valid)
{
	isHandTrackingValid = valid;
}

const InteractionProfile* XrController::GetInteractionProfile()
{
	return &profile;
}
