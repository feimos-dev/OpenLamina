//
// Like xrutil.h but not included in every file by default
//
// Created by ZNix on 15/03/2021.
//

#pragma once

#include "generated/interfaces/vrtypes.h"
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <openxr/openxr.h>
#include <optional>

namespace xr_utils {

// The time to locate devices at for a pose asked `secondsFromNow` ahead, given the time now and the display time of the
// current frame (zero where unknown). A time within a few milliseconds of the frame's is the frame's. Zero means the runtime
// can't say what time it is, and the caller keeps its default.
XrTime PoseTimeForPrediction(float secondsFromNow, XrTime now, XrTime frameTime);

// The time to locate a device at: `requested` where the caller names one; otherwise, for the latest tracking data
// (latest), the time now, and for a frame's poses the display time of the next frame (frameTime). Either falls back on
// the other where it's unknown (zero or one); zero if neither is known, and the caller keeps its default.
XrTime PoseTimeForState(bool latest, XrTime requested, XrTime now, XrTime frameTime);

// Moves two eyes apart by offset (closer together for a negative one), each by half of it along the line between them;
// eyes at the same place stay
void WidenEyes(XrPosef& left, XrPosef& right, float offset);

// A view turned to look where the head looks, for a game that renders both eyes facing straight ahead: `eyeInHead` is
// the view's orientation in the head. The field of view grows to take in all of the view's. A view that turns so far
// that part of it is behind the head stays as it is, and the answer is false.
bool ParallelView(XrView& view, const XrQuaternionf& eyeInHead);

// How much wider (x) and taller (y) `to` is than `from`, measured on the z = -1 plane: the image size that keeps a
// view's detail when its field of view changes from one to the other
glm::vec2 TangentSpanScale(const XrFovf& from, const XrFovf& to);

// A point given as tangents on a view's z = -1 plane, as the head sees it through ParallelView's view: unchanged where
// it's behind the head
XrVector2f ParallelTangents(const XrVector2f& tangents, const XrQuaternionf& eyeInHead);

// The heading of a pose's forward direction as a rotation about the vertical axis, ignoring pitch and roll
XrQuaternionf YawOnly(const XrQuaternionf& orientation);

// A seated origin placed at a head: `origin` is the current origin's pose in its parent space and `head` the head's pose
// in the current origin. The result keeps the head's position and heading, and stays upright.
XrPosef SeatedOriginAtHead(const XrPosef& origin, const XrPosef& head);

bool PoseFromSpace(vr::TrackedDevicePose_t* pose, XrSpace space, vr::ETrackingUniverseOrigin origin,
    std::optional<glm::mat4> extraTransform = {}, XrTime time = 0);
// The pose of a controller held where the tracked hand's palm is; false, with the pose left as it was, for a hand
// that isn't tracked
bool PoseFromHandTracking(vr::TrackedDevicePose_t* pose, XrHandJointLocationsEXT locations, XrHandJointVelocitiesEXT velocities, bool isRight);
} // namespace xr_utils
