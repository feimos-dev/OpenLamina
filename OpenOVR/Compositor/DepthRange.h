#pragma once
// The near and far planes of a game's projection, for the depth it submits

#include "generated/interfaces/vrtypes.h"

// OpenVR's projection (D3D convention, z in 0..1); reversed-Z and infinite far handled; false for a matrix that isn't a
// perspective projection. With m[2][2] = f/(n-f) and m[2][3] = n*f/(n-f): n = m23/m22 and f = m23/(m22+1);
// m[2][2] == -1 is an infinite far plane (farZ = +INFINITY), and a reversed one (n > f) is passed through as such.
bool DepthRangeFromProjection(const vr::HmdMatrix44_t& m, float& nearZ, float& farZ);
