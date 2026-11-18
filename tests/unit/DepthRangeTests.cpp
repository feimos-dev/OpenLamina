// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

#include "Compositor/DepthRange.h"

#include <cmath>

namespace {

// OpenVR's projection for a symmetric 90 degree field of view, D3D convention (z in 0..1), from near and far as
// GetProjectionMatrix makes it
vr::HmdMatrix44_t Projection(float nearZ, float farZ)
{
	vr::HmdMatrix44_t m{};
	m.m[0][0] = 1;
	m.m[1][1] = 1;
	m.m[2][2] = farZ / (nearZ - farZ);
	m.m[2][3] = nearZ * farZ / (nearZ - farZ);
	m.m[3][2] = -1;
	return m;
}

} // namespace

TEST_CASE("Depth_range_round_trip")
{
	float n = 0, f = 0;
	REQUIRE(DepthRangeFromProjection(Projection(0.1f, 100.0f), n, f));
	CHECK(std::fabs(n - 0.1f) < 1e-4f);
	CHECK(std::fabs(f - 100.0f) < 1e-4f * 100.0f);
}

TEST_CASE("Depth_range_infinite_far")
{
	// m22 = -1, m23 = -n
	vr::HmdMatrix44_t m = Projection(0.1f, 100.0f);
	m.m[2][2] = -1;
	m.m[2][3] = -0.1f;
	float n = 0, f = 0;
	REQUIRE(DepthRangeFromProjection(m, n, f));
	CHECK(std::fabs(n - 0.1f) < 1e-4f);
	CHECK(std::isinf(f));
	CHECK(f > 0);
}

TEST_CASE("Depth_range_reversed")
{
	// Reversed Z: the near plane at z = 1, the far at 0; passed through as nearZ > farZ
	float n = 0, f = 0;
	REQUIRE(DepthRangeFromProjection(Projection(100.0f, 0.1f), n, f));
	CHECK(std::fabs(n - 100.0f) < 1e-2f);
	CHECK(std::fabs(f - 0.1f) < 1e-4f);
	// Reversed with an infinite far plane: m22 = 0, m23 = n
	vr::HmdMatrix44_t m = Projection(0.1f, 100.0f);
	m.m[2][2] = 0;
	m.m[2][3] = 0.1f;
	REQUIRE(DepthRangeFromProjection(m, n, f));
	CHECK(std::isinf(n));
	CHECK(std::fabs(f - 0.1f) < 1e-4f);
}

TEST_CASE("Depth_range_not_perspective_false")
{
	// Orthographic: no -1 in the last row
	vr::HmdMatrix44_t m{};
	m.m[0][0] = 1;
	m.m[1][1] = 1;
	m.m[2][2] = -0.01f;
	m.m[2][3] = 0;
	m.m[3][3] = 1;
	float n = 0, f = 0;
	CHECK_FALSE(DepthRangeFromProjection(m, n, f));
	// Nothing at all
	CHECK_FALSE(DepthRangeFromProjection(vr::HmdMatrix44_t{}, n, f));
}
