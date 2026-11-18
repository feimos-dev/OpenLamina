// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

#include "Misc/Bounds.h"

TEST_CASE("Bounds_alpha_linear")
{
	CHECK(BoundsAlpha(0.0f) == doctest::Approx(1.0f));
	CHECK(BoundsAlpha(0.1f) == doctest::Approx(0.75f));
	CHECK(BoundsAlpha(0.2f) == doctest::Approx(0.5f));
	CHECK(BoundsAlpha(0.3f) == doctest::Approx(0.25f));
	CHECK(BoundsAlpha(0.4f) == doctest::Approx(0.0f));
	CHECK(BoundsAlpha(1.0f) == doctest::Approx(0.0f));
	// Beyond the edge: all there
	CHECK(BoundsAlpha(-0.3f) == doctest::Approx(1.0f));
}

TEST_CASE("Bounds_wall_near_one_edge")
{
	// A 2 x 3 m play area, the HMD 0.2 m from its +X edge, a controller in the middle
	std::vector<BoundsWall> walls = BoundsWalls(2, 3, { { 0.8f, 1.7f, 0 }, { 0, 1, 0 } });
	REQUIRE(walls.size() == 1);
	CHECK(walls[0].edge == 0);
	CHECK(walls[0].alpha == doctest::Approx(0.5f));
	CHECK(walls[0].pose.position.x == doctest::Approx(1.0f));
	CHECK(walls[0].width == doctest::Approx(3.0f));
	// The wall faces into the play area: its +Z, turned by the pose, points along -X
	const XrQuaternionf& q = walls[0].pose.orientation;
	const float zx = 2 * (q.x * q.z + q.w * q.y);
	CHECK(zx == doctest::Approx(-1.0f));

	// In a corner: two walls; in the middle: none
	CHECK(BoundsWalls(2, 3, { { -0.9f, 1.7f, -1.4f } }).size() == 2);
	CHECK(BoundsWalls(2, 3, { { 0, 1.7f, 0 } }).empty());
	CHECK(BoundsWalls(0, 3, { { 0.9f, 1.7f, 0 } }).empty());
}
