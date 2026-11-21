//
// Created by ZNix on 15/03/2021.
//

#include "stdafx.h"

#include "xrmoreutils.h"
#include <convert.h>
#include <cmath>

XrTime xr_utils::PoseTimeForPrediction(float secondsFromNow, XrTime now, XrTime frameTime)
{
	if (now <= 0)
		return 0;

	// Not NaN, and within a range a runtime can predict
	if (!(secondsFromNow > -0.1f))
		secondsFromNow = -0.1f;
	if (secondsFromNow > 1.0f)
		secondsFromNow = 1.0f;

	XrTime target = now + (XrTime)std::llround((double)secondsFromNow * 1e9);
	constexpr XrTime frameTolerance = 3000000; // 3 ms
	if (frameTime > 1 && target - frameTime <= frameTolerance && frameTime - target <= frameTolerance)
		return frameTime;
	return target;
}

XrTime xr_utils::PoseTimeForState(bool latest, XrTime requested, XrTime now, XrTime frameTime)
{
	if (requested > 0)
		return requested;
	const XrTime first = latest ? now : frameTime;
	const XrTime second = latest ? frameTime : now;
	if (first > 1)
		return first;
	if (second > 1)
		return second;
	return 0;
}

void xr_utils::WidenEyes(XrPosef& left, XrPosef& right, float offset)
{
	glm::vec3 l = X2G_v3f(left.position), r = X2G_v3f(right.position);
	glm::vec3 apart = r - l;
	if (offset == 0.0f || !(glm::length(apart) > 1e-6f))
		return;
	glm::vec3 half = glm::normalize(apart) * (offset / 2);
	left.position = G2X_v3f(l - half);
	right.position = G2X_v3f(r + half);
}

XrQuaternionf xr_utils::YawOnly(const XrQuaternionf& orientation)
{
	glm::vec3 forward = X2G_quat(orientation) * glm::vec3(0.0f, 0.0f, -1.0f);
	// Looking straight up or down has no heading
	if (forward.x * forward.x + forward.z * forward.z < 1e-6f)
		return { 0.0f, 0.0f, 0.0f, 1.0f };

	float yaw = atan2f(-forward.x, -forward.z);
	return { 0.0f, sinf(yaw / 2), 0.0f, cosf(yaw / 2) };
}

XrPosef xr_utils::SeatedOriginAtHead(const XrPosef& origin, const XrPosef& head)
{
	glm::quat originRotation = X2G_quat(origin.orientation);
	glm::vec3 position = X2G_v3f(origin.position) + originRotation * X2G_v3f(head.position);
	glm::quat rotation = glm::normalize(originRotation * X2G_quat(YawOnly(head.orientation)));
	return { G2X_quat(rotation), G2X_v3f(position) };
}

bool xr_utils::PoseFromSpace(vr::TrackedDevicePose_t* pose, XrSpace space, vr::ETrackingUniverseOrigin origin,
    std::optional<glm::mat4> extraTransform, XrTime time)
{
	auto sessionLock = xr_session.lock_shared(); // held while reading: a session restart waits for it
	if (!xr_gbl) {
		// No session: no pose
		if (pose) {
			*pose = {};
			pose->eTrackingResult = vr::TrackingResult_Uninitialized;
		}
		return false;
	}
	auto baseSpace = xr_space_from_tracking_origin(origin);
	// A space the runtime wouldn't make (see XrSessionGlobals) has no pose in it: the runtime isn't asked with no handle
	if (space == XR_NULL_HANDLE || baseSpace == XR_NULL_HANDLE)
		return false;

	XrSpaceVelocity velocity{ XR_TYPE_SPACE_VELOCITY };
	XrSpaceLocation info{ XR_TYPE_SPACE_LOCATION, &velocity, 0, {} };

	OOVR_FAILED_XR_SOFT_ABORT(xrLocateSpace(space, baseSpace, time > 0 ? time : xr_gbl->GetBestTime(), &info));
	
	// A pose whose orientation is tracked is given, marked valid only when its position is tracked too
	bool orientationTracked = (info.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
	if (!orientationTracked)
		return false;

	glm::mat4 mat = X2G_om34_pose(info.pose);

	// Apply the extra transform if required - this is applied first, since it's used to swap between the
	// grip and steamvr hand spaces.
	if (extraTransform) {
		mat = mat * extraTransform.value();
	}

	pose->bDeviceIsConnected = true;
	pose->bPoseIsValid = (info.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
	pose->mDeviceToAbsoluteTracking = G2S_m34(mat);
	pose->eTrackingResult = pose->bPoseIsValid ? vr::TrackingResult_Running_OK : vr::TrackingResult_Running_OutOfRange;
	// The velocities, in the tracking space, where the runtime gives them (zero otherwise). The angular one is the same
	// everywhere on the device; the linear one is the space's, and the extra transform moves the pose away from the
	// space's point, which then also moves with the turn.
	glm::vec3 linear(0.0f), angular(0.0f);
	if (velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT)
		angular = X2G_v3f(velocity.angularVelocity);
	if (velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) {
		linear = X2G_v3f(velocity.linearVelocity);
		if (extraTransform)
			linear += glm::cross(angular, glm::vec3(mat[3]) - X2G_v3f(info.pose.position));
	}
	pose->vVelocity = G2S_v3f(linear);
	pose->vAngularVelocity = G2S_v3f(angular);
	return true;
}

bool xr_utils::PoseFromHandTracking(vr::TrackedDevicePose_t* pose, XrHandJointLocationsEXT locations, XrHandJointVelocitiesEXT velocities, bool isRight)
{
	const int boneToUse = XR_HAND_JOINT_PALM_EXT;

	XrHandJointLocationEXT palmJoint = locations.jointLocations[boneToUse];
	XrHandJointVelocityEXT velocity = velocities.jointVelocities[boneToUse];

	// A hand whose palm orientation is tracked gives a pose
	bool orientationTracked = (palmJoint.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;

	if (!locations.isActive || !orientationTracked) {
		return false;
	}

	glm::mat4 gripPoseMatrix = X2G_om34_pose(palmJoint.pose);

	glm::mat4 transform = GetMat4x4FromOriginAndEulerRotations(
	    {
	        isRight ? -0.09 : 0.09,
	        -0.03,
	        -0.09,
	    },
	    { 0.0, isRight ? 45.0 : -45.0, isRight ? 90.0 : -90.0 });

	gripPoseMatrix = gripPoseMatrix * transform;

	pose->mDeviceToAbsoluteTracking = G2S_m34(gripPoseMatrix);

	pose->bDeviceIsConnected = true;
	// Valid only with the palm's position tracked too, as a pose from a space is
	pose->bPoseIsValid = (palmJoint.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
	pose->eTrackingResult = pose->bPoseIsValid ? vr::TrackingResult_Running_OK : vr::TrackingResult_Running_OutOfRange;
	// The velocities where the runtime gives them (zero otherwise); no offsetting transform, they are in world space
	const XrVector3f still{ 0, 0, 0 };
	pose->vVelocity = X2S_v3f((velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) ? velocity.linearVelocity : still);
	pose->vAngularVelocity = X2S_v3f((velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) ? velocity.angularVelocity : still);
	return true;
}
