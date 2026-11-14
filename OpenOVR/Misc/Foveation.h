#pragma once

#include "generated/interfaces/vrtypes.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <optional>

namespace Foveation {

/**
 * Where a direction in a view's frame (-Z forward, +Y up) lands through that view's projection, in normalised device
 * coordinates clamped to -1..1. Nothing for a direction behind the view or along its plane.
 */
std::optional<vr::HmdVector2_t> CenterForProjection(const glm::vec3& direction, const vr::HmdMatrix44_t& projection);

/**
 * Which eye's projection a matrix is nearer to, 0 (left) or 1 (right), by the terms that place x and y (so a flipped
 * y or a jittered matrix still finds its eye). -1 if it is as near to both.
 */
int EyeOfProjection(const vr::HmdMatrix44_t& projection, const vr::HmdMatrix44_t& left, const vr::HmdMatrix44_t& right);

/** The direction a gaze (given in the head's frame) looks in, in an eye's frame */
glm::vec3 GazeInEye(const glm::quat& gazeInHead, const glm::quat& eyeInHead);

} // namespace Foveation
