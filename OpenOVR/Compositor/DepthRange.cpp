#include "stdafx.h"

#include "DepthRange.h"

#include <cmath>

bool DepthRangeFromProjection(const vr::HmdMatrix44_t& m, float& nearZ, float& farZ)
{
	// A perspective projection divides by -z: its last row is (0, 0, -1, 0)
	if (m.m[3][2] != -1.0f || m.m[3][3] != 0.0f || m.m[3][0] != 0.0f || m.m[3][1] != 0.0f)
		return false;
	const float m22 = m.m[2][2], m23 = m.m[2][3];
	if (!std::isfinite(m22) || !std::isfinite(m23) || m23 == 0.0f)
		return false;

	// m22 = f/(n-f) and m23 = n*f/(n-f): n = m23/m22 and f = m23/(m22+1). m22 == -1 is a far plane at infinity, and
	// m22 == 0 a reversed projection's near plane at infinity (z = n/-z_view: m23 = n)
	if (m22 == -1.0f) {
		nearZ = -m23;
		farZ = INFINITY;
	} else if (m22 == 0.0f) {
		nearZ = INFINITY;
		farZ = m23;
	} else {
		nearZ = m23 / m22;
		farZ = m23 / (m22 + 1.0f);
	}
	if (m22 == -1.0f && nearZ > 0)
		return true;
	// Both planes in front of the eye and apart (reversed, n > f, is passed through as it is)
	return std::isfinite(nearZ) ? (nearZ > 0 && farZ > 0 && nearZ != farZ) : farZ > 0;
}
