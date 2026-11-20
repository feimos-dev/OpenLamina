// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

#include "Misc/Bounds.h"
#include "Reimpl/BaseChaperoneSetup.h"

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

TEST_CASE("Bounds_play_area_falls_back_to_the_room_setup")
{
	Json::Value chaperone;
	chaperone["play_area"].append(3.0f);
	chaperone["play_area"].append(2.0f);
	// The runtime's stage bounds win
	const XrExtent2Df stage{ 4.0f, 5.0f };
	XrExtent2Df area = BoundsPlayArea(&stage, chaperone);
	CHECK(area.width == doctest::Approx(4.0f));
	CHECK(area.height == doctest::Approx(5.0f));
	// Without them, the committed room setup's play area
	area = BoundsPlayArea(nullptr, chaperone);
	CHECK(area.width == doctest::Approx(3.0f));
	CHECK(area.height == doctest::Approx(2.0f));
	// Without either, none
	area = BoundsPlayArea(nullptr, Json::Value());
	CHECK(area.width == 0.0f);
	CHECK(area.height == 0.0f);
}

TEST_CASE("Bounds_preview_shows_every_wall_whole")
{
	// Nobody near any edge: no walls, unless previewed
	const std::vector<XrVector3f> centre = { { 0, 1.7f, 0 } };
	CHECK(BoundsWalls(4, 3, centre).empty());
	const std::vector<BoundsWall> walls = BoundsWalls(4, 3, centre, true);
	REQUIRE(walls.size() == 4);
	for (const BoundsWall& w : walls)
		CHECK(w.alpha == 1.0f);
	CHECK(walls[0].width == 3.0f); // the X edges are as long as the area is deep
	CHECK(walls[2].width == 4.0f);
	CHECK(BoundsWalls(0, 3, centre, true).empty()); // no play area set yet
}

TEST_CASE("Working_set_preview_is_shown_until_hidden")
{
	BaseChaperoneSetup setup;
	CHECK_FALSE(setup.PreviewedPlayArea());
	setup.SetWorkingPlayAreaSize(3.5f, 2.5f);
	setup.ShowWorkingSetPreview();
	REQUIRE(setup.PreviewedPlayArea());
	CHECK(setup.PreviewedPlayArea()->first == 3.5f);
	CHECK(setup.PreviewedPlayArea()->second == 2.5f);
	// It follows the working copy
	setup.SetWorkingPlayAreaSize(4.0f, 2.5f);
	CHECK(setup.PreviewedPlayArea()->first == 4.0f);
	setup.HideWorkingSetPreview();
	CHECK_FALSE(setup.PreviewedPlayArea());
}
