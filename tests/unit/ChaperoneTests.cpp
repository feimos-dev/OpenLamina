// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

#include "Misc/ChaperoneData.h"
#include "Reimpl/BaseSystem.h"

#include <glm/gtc/quaternion.hpp>

TEST_CASE("Chaperone_data_round_trip")
{
	ChaperoneData d = ChaperoneData::FromStage(4, 5);
	REQUIRE(d.collisionBounds.size() == 4);
	CHECK(d.collisionBounds[0].vCorners[0].v[0] == 2.0f);
	CHECK(d.collisionBounds[2].vCorners[2].v[2] == 2.5f);
	d.tags = { 1, 0 };
	d.physicalBounds.push_back(ChaperoneData::FloorQuad(6, 7));
	d.perimeter = { { { 1, 2 } }, { { -3, 4.5f } } };
	d.standingZero.m[0][3] = 0.25f;
	d.seatedZero.m[1][3] = 1.5f;
	d.colors[0] = { 1, 0.5f, 0, 1 };
	d.colors[1] = { 0, 0, 1, 0.5f };

	std::optional<ChaperoneData> back = ChaperoneData::FromJson(d.ToJson());
	REQUIRE(back.has_value());
	CHECK(back->playArea[0] == 4.0f);
	CHECK(back->playArea[1] == 5.0f);
	REQUIRE(back->collisionBounds.size() == 4);
	for (int c = 0; c < 4; c++)
		for (int i = 0; i < 3; i++)
			CHECK(back->collisionBounds[0].vCorners[c].v[i] == d.collisionBounds[0].vCorners[c].v[i]);
	CHECK(back->tags == d.tags);
	REQUIRE(back->physicalBounds.size() == 1);
	CHECK(back->physicalBounds[0].vCorners[2].v[0] == -3.0f);
	REQUIRE(back->perimeter.size() == 2);
	CHECK(back->perimeter[1].v[0] == -3.0f);
	CHECK(back->perimeter[1].v[1] == 4.5f);
	CHECK(back->standingZero.m[0][3] == 0.25f);
	CHECK(back->standingZero.m[0][0] == 1.0f);
	CHECK(back->seatedZero.m[1][3] == 1.5f);
	CHECK(back->colors[0].g == 0.5f);
	CHECK(back->colors[1].a == 0.5f);

	// Not a room setup
	CHECK_FALSE(ChaperoneData::FromJson(Json::Value(Json::objectValue)).has_value());
	CHECK_FALSE(ChaperoneData::FromJson(Json::Value("garbage")).has_value());
	// A broken quad is left out, not half read
	Json::Value broken = d.ToJson();
	broken["collision_bounds"][0][1] = "x";
	CHECK(ChaperoneData::FromJson(broken)->collisionBounds.size() == 3);
	// A tag that isn't a number (an imported buffer's, say) reads as 0
	broken["tags"][0] = "x";
	broken["tags"][1] = -1;
	CHECK(ChaperoneData::FromJson(broken)->tags == std::vector<uint8_t>{ 0, 0 });
}

TEST_CASE("Chaperone_floor_quad_starts_at_the_front_right")
{
	// Counter-clockwise seen from above, from (+x, -z)
	const vr::HmdQuad_t q = ChaperoneData::FloorQuad(2, 4);
	const float expected[4][2] = { { 1, -2 }, { -1, -2 }, { -1, 2 }, { 1, 2 } };
	for (int i = 0; i < 4; i++) {
		CHECK(q.vCorners[i].v[0] == expected[i][0]);
		CHECK(q.vCorners[i].v[1] == 0.0f);
		CHECK(q.vCorners[i].v[2] == expected[i][1]);
	}
}

TEST_CASE("Chaperone_walls_stand_on_the_edges_of_the_floor_quad")
{
	const vr::HmdQuad_t floor = ChaperoneData::FloorQuad(2, 4);
	const std::vector<vr::HmdQuad_t> walls = ChaperoneData::WallQuads(2, 4, 2.5f);
	REQUIRE(walls.size() == 4);
	for (int i = 0; i < 4; i++) {
		const vr::HmdVector3_t from = floor.vCorners[i], to = floor.vCorners[(i + 1) % 4];
		const vr::HmdQuad_t& w = walls[i];
		for (int c : { 0, 1 }) {
			CHECK(w.vCorners[c].v[0] == from.v[0]);
			CHECK(w.vCorners[c].v[2] == from.v[2]);
		}
		for (int c : { 2, 3 }) {
			CHECK(w.vCorners[c].v[0] == to.v[0]);
			CHECK(w.vCorners[c].v[2] == to.v[2]);
		}
		CHECK(w.vCorners[0].v[1] == 0.0f);
		CHECK(w.vCorners[3].v[1] == 0.0f);
		CHECK(w.vCorners[1].v[1] == 2.5f);
		CHECK(w.vCorners[2].v[1] == 2.5f);
	}
	// A room of the runtime's stage has a wall on each edge
	CHECK(ChaperoneData::FromStage(1, 1).collisionBounds.size() == 4);
}

TEST_CASE("Chaperone_default_colours_are_clear_bounds_and_a_clear_white_camera")
{
	const ChaperoneData d;
	CHECK(d.colors[0].a == 0.0f);
	CHECK(d.colors[1].r == 1.0f);
	CHECK(d.colors[1].g == 1.0f);
	CHECK(d.colors[1].b == 1.0f);
	CHECK(d.colors[1].a == 0.0f);
	// A committed room that has no colours keeps them
	Json::Value room = d.ToJson();
	room.removeMember("colors");
	REQUIRE(ChaperoneData::FromJson(room).has_value());
	CHECK(ChaperoneData::FromJson(room)->colors[1].b == 1.0f);
}

TEST_CASE("Chaperone_seated_zero_is_kept_in_the_offsets_document")
{
	Json::Value offsets(Json::objectValue);
	offsets["stage"]["zero"] = 1; // another record stays
	CHECK_FALSE(BaseSystem::_SeatedZeroOf(offsets).has_value());
	CHECK_FALSE(BaseSystem::_SeatedZeroOf(Json::Value()).has_value());

	vr::HmdMatrix34_t zero{};
	zero.m[0][0] = zero.m[1][1] = zero.m[2][2] = 1;
	zero.m[0][3] = 0.5f;
	zero.m[1][3] = 1.2f;
	BaseSystem::_RecordSeatedZero(offsets, zero);
	std::optional<vr::HmdMatrix34_t> read = BaseSystem::_SeatedZeroOf(offsets);
	REQUIRE(read.has_value());
	CHECK(read->m[0][3] == doctest::Approx(0.5f));
	CHECK(read->m[1][3] == doctest::Approx(1.2f));
	CHECK(read->m[2][2] == doctest::Approx(1.0f));
	CHECK(offsets["stage"]["zero"] == 1);
}

TEST_CASE("Chaperone_seated_zero_goes_to_the_head_facing_its_way")
{
	// The head 1.6 m up and 1 m to the side, turned 90 degrees left and looking down a little
	glm::quat turned = glm::angleAxis(1.5707963f, glm::vec3(0, 1, 0)) * glm::angleAxis(-0.3f, glm::vec3(1, 0, 0));
	XrPosef head{ { turned.x, turned.y, turned.z, turned.w }, { 1.0f, 1.6f, 0.0f } };
	XrPosef zero = BaseSystem::_SeatedZeroAtHead(head);
	CHECK(zero.position.x == doctest::Approx(1.0f));
	CHECK(zero.position.y == doctest::Approx(1.6f));
	// Only the heading: a quarter turn about the vertical
	glm::quat q(zero.orientation.w, zero.orientation.x, zero.orientation.y, zero.orientation.z);
	glm::vec3 forward = q * glm::vec3(0, 0, -1);
	CHECK(forward.x == doctest::Approx(-1.0f).epsilon(0.001));
	CHECK(forward.y == doctest::Approx(0.0f));
}
