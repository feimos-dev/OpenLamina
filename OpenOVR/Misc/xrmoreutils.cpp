//
// Created by ZNix on 15/03/2021.
//

#include "stdafx.h"

#include "xrmoreutils.h"
#include <convert.h>

bool xr_utils::PoseFromSpace(vr::TrackedDevicePose_t* pose, XrSpace space, vr::ETrackingUniverseOrigin origin,
    std::optional<glm::mat4> extraTransform)
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

	XrSpaceVelocity velocity{ XR_TYPE_SPACE_VELOCITY };
	XrSpaceLocation info{ XR_TYPE_SPACE_LOCATION, &velocity, 0, {} };

	OOVR_FAILED_XR_SOFT_ABORT(xrLocateSpace(space, baseSpace, xr_gbl->GetBestTime(), &info));
	
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

void xr_utils::PoseFromHandTracking(vr::TrackedDevicePose_t* pose, XrHandJointLocationsEXT locations, XrHandJointVelocitiesEXT velocities, bool isRight)
{
	const int boneToUse = XR_HAND_JOINT_PALM_EXT;

	XrHandJointLocationEXT palmJoint = locations.jointLocations[boneToUse];
	XrHandJointVelocityEXT velocity = velocities.jointVelocities[boneToUse];

	// A pose whose orientation is tracked is given, marked valid only when its position is tracked too
	bool orientationTracked = (palmJoint.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;

	if (!locations.isActive || !orientationTracked) {
		return;
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
	pose->bPoseIsValid = true;
	pose->eTrackingResult = pose->bPoseIsValid ? vr::TrackingResult_Running_OK : vr::TrackingResult_Running_OutOfRange;
	pose->vVelocity = X2S_v3f(velocity.linearVelocity); // No offsetting transform - this is in world-space
	pose->vAngularVelocity = X2S_v3f(velocity.angularVelocity); // TODO find out if this needs a transform
}
