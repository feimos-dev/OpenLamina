// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

#include "Misc/OverlayMaths.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <map>

using namespace overlay_maths;

namespace {

const float kPi = 3.14159265358979f;

XrPosef At(float x, float y, float z)
{
	return XrPosef{ { 0, 0, 0, 1 }, { x, y, z } };
}

// A rotation about +Y by angle radians
XrQuaternionf AboutY(float angle)
{
	return XrQuaternionf{ 0, std::sin(angle / 2), 0, std::cos(angle / 2) };
}

} // namespace

TEST_CASE("Intersect_quad_centre")
{
	Placement p{ At(0, 0, -2), 1, 1, 0 };
	Hit hit{};
	REQUIRE(Intersect(p, { 0, 0, 0 }, { 0, 0, -1 }, hit));
	CHECK(hit.u == doctest::Approx(0.5f));
	CHECK(hit.v == doctest::Approx(0.5f));
	CHECK(hit.distance == doctest::Approx(2.0f));
	CHECK(hit.point.z == doctest::Approx(-2.0f));
	CHECK(hit.normal.z == doctest::Approx(1.0f));
}

TEST_CASE("Intersect_quad_corner_uv")
{
	// v grows upwards: (0, 0) is the lower left
	Placement p{ At(0, 0, -2), 2, 1, 0 };
	Hit hit{};
	REQUIRE(Intersect(p, { 0.5f, 0.25f, 0 }, { 0, 0, -1 }, hit));
	CHECK(hit.u == doctest::Approx(0.75f));
	CHECK(hit.v == doctest::Approx(0.75f));
	// Off the overlay: filled in, but not a hit
	Hit off{};
	CHECK_FALSE(Intersect(p, { 1.5f, 0, 0 }, { 0, 0, -1 }, off));
	CHECK(off.u == doctest::Approx(1.25f));
}

TEST_CASE("Intersect_quad_miss_behind")
{
	Placement p{ At(0, 0, -2), 1, 1, 0 };
	Hit hit{};
	CHECK_FALSE(Intersect(p, { 0, 0, 0 }, { 0, 0, 1 }, hit));
	// From behind the overlay, towards it
	CHECK_FALSE(Intersect(p, { 0, 0, -3 }, { 0, 0, 1 }, hit));
}

TEST_CASE("Intersect_cylinder_edge")
{
	// Half a circle: the overlay's 1 m wraps around a cylinder of radius 1/π, whose axis is a radius in front of it
	const float radius = CylinderRadius(1, 0.5f);
	CHECK(radius == doctest::Approx(1 / kPi));
	CHECK(CylinderAngle(0.5f) == doctest::Approx(kPi));
	Placement p{ At(0, 0, -1), 1, 1, 0.5f };
	const XrVector3f axis{ 0, 0, -1 + radius };
	// Towards the arc 0.48 π right of its centre: 0.48 of the half circle's π, so u = 0.5 + 0.48
	const float angle = 0.48f * kPi;
	Hit hit{};
	REQUIRE(Intersect(p, axis, { std::sin(angle), 0, -std::cos(angle) }, hit));
	CHECK(hit.u == doctest::Approx(0.98f).epsilon(1e-3));
	CHECK(hit.v == doctest::Approx(0.5f));
	CHECK(hit.distance == doctest::Approx(radius));
	// The normal faces the axis
	CHECK(hit.normal.x == doctest::Approx(-std::sin(angle)));
	// Past the arc's edge: no hit
	const float past = 0.52f * kPi;
	CHECK_FALSE(Intersect(p, axis, { std::sin(past), 0, -std::cos(past) }, hit));
	// From in front of the overlay, straight at its centre
	REQUIRE(Intersect(p, { 0, 0, 0 }, { 0, 0, -1 }, hit));
	CHECK(hit.u == doctest::Approx(0.5f));
	CHECK(hit.distance == doctest::Approx(1.0f));
}

TEST_CASE("Mask_circle")
{
	std::vector<OOVR_VROverlayIntersectionMaskPrimitive_t> mask(1);
	mask[0].m_nPrimitiveType = OverlayIntersectionPrimitiveType_Circle;
	mask[0].m_Primitive.m_Circle = { 0.5f, 0.5f, 0.1f };
	CHECK(InMask(mask, 0.55f, 0.5f));
	CHECK_FALSE(InMask(mask, 0.7f, 0.5f));
	CHECK(InMask({}, 0.7f, 0.5f));

	OOVR_VROverlayIntersectionMaskPrimitive_t rect{};
	rect.m_nPrimitiveType = OverlayIntersectionPrimitiveType_Rectangle;
	rect.m_Primitive.m_Rectangle = { 0.6f, 0.4f, 0.2f, 0.2f };
	mask.push_back(rect);
	CHECK(InMask(mask, 0.7f, 0.5f));
	CHECK_FALSE(InMask(mask, 0.7f, 0.65f));
}

TEST_CASE("Change_origin_seated_to_standing")
{
	// The seated origin 1 m along standing +Z, turned 90 degrees left
	const XrPosef seated{ AboutY(kPi / 2), { 0, 0, 1 } };
	const XrPosef pose = At(1, 0, 0);
	XrPosef standing = ChangeOrigin(pose, vr::TrackingUniverseSeated, vr::TrackingUniverseStanding, seated);
	// Seated +X is standing -Z
	CHECK(standing.position.x == doctest::Approx(0).epsilon(1e-5));
	CHECK(standing.position.z == doctest::Approx(0).epsilon(1e-5));
	CHECK(standing.orientation.y == doctest::Approx(AboutY(kPi / 2).y));
	XrPosef back = ChangeOrigin(standing, vr::TrackingUniverseStanding, vr::TrackingUniverseSeated, seated);
	CHECK(back.position.x == doctest::Approx(1));
	CHECK(back.position.z == doctest::Approx(0).epsilon(1e-5));
	CHECK(back.orientation.w == doctest::Approx(1));
	// Raw is standing, and the same origin changes nothing
	XrPosef raw = ChangeOrigin(pose, vr::TrackingUniverseRawAndUncalibrated, vr::TrackingUniverseStanding, seated);
	CHECK(raw.position.x == doctest::Approx(1));
	XrPosef same = ChangeOrigin(pose, vr::TrackingUniverseSeated, vr::TrackingUniverseSeated, seated);
	CHECK(same.position.x == doctest::Approx(1));
}

TEST_CASE("Relative_chain_cycle_hidden")
{
	// Each overlay is a number: n is relative to links[n], or placed itself where links has none
	std::map<int, int> links;
	glm::mat4 root = glm::translate(glm::mat4(1.0f), glm::vec3(1, 0, 0)) * glm::rotate(glm::mat4(1.0f), kPi / 2, glm::vec3(0, 1, 0));
	auto link = [&](const void* o) {
		ChainLink l;
		int n = (int)(intptr_t)o;
		if (n == 0) // no such overlay
			return l;
		auto it = links.find(n);
		if (it == links.end()) {
			l.placed = true;
			l.transform = root;
			return l;
		}
		l.relative = true;
		l.parent = (const void*)(intptr_t)it->second;
		l.transform = glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, -1));
		return l;
	};
	glm::mat4 out(0.0f);

	// 1 is relative to 2, which is placed: 1 m along 2's -Z, which is turned to standing -X
	links = { { 1, 2 } };
	REQUIRE(ResolveChain((const void*)1, link, out) == Chain::Placed);
	CHECK(out[3][0] == doctest::Approx(0).epsilon(1e-5));
	CHECK(out[3][2] == doctest::Approx(0).epsilon(1e-5));

	// 1 and 2 relative to each other
	links = { { 1, 2 }, { 2, 1 } };
	CHECK(ResolveChain((const void*)1, link, out) == Chain::Cycle);
	// 1 relative to itself
	links = { { 1, 1 } };
	CHECK(ResolveChain((const void*)1, link, out) == Chain::Cycle);

	// Eight links deep is placed, nine is too deep
	links.clear();
	for (int i = 1; i <= 8; i++)
		links[i] = i + 1;
	CHECK(ResolveChain((const void*)1, link, out) == Chain::Placed);
	links[9] = 10;
	CHECK(ResolveChain((const void*)1, link, out) == Chain::TooDeep);

	// A parent that isn't there
	links = { { 1, 0 } };
	CHECK(ResolveChain((const void*)1, link, out) == Chain::Unplaced);
}

TEST_CASE("Projection_quad_covers_the_eye")
{
	// Tangents as GetProjectionRaw's: 1 left, 0.5 right, 1 up (top -1), 0.5 down
	Placement p = ProjectionQuad(At(0, 1.7f, 0), -1, 0.5f, -1, 0.5f);
	CHECK(p.width == doctest::Approx(1.5f));
	CHECK(p.height == doctest::Approx(1.5f));
	CHECK(p.curvature == 0);
	CHECK(p.pose.position.x == doctest::Approx(-0.25f));
	CHECK(p.pose.position.y == doctest::Approx(1.95f));
	CHECK(p.pose.position.z == doctest::Approx(-1));
	// Turned with the eye
	Placement turned = ProjectionQuad(XrPosef{ AboutY(kPi / 2), { 0, 0, 0 } }, -1, 1, -1, 1);
	CHECK(turned.pose.position.x == doctest::Approx(-1));
	CHECK(turned.pose.orientation.y == doctest::Approx(AboutY(kPi / 2).y));
}

TEST_CASE("Point_on_overlay_and_cursor_hotspot")
{
	Placement p{ At(0, 1, -2), 2, 1, 0 };
	XrPosef corner = PointOn(p, 0, 0);
	CHECK(corner.position.x == doctest::Approx(-1));
	CHECK(corner.position.y == doctest::Approx(0.5f));
	CHECK(corner.position.z == doctest::Approx(-2));
	// On a cylinder: the arc's right end, turned to face the axis
	Placement curved{ At(0, 0, -1), 1, 1, 0.5f };
	XrPosef end = PointOn(curved, 1, 0.5f);
	const float radius = CylinderRadius(1, 0.5f);
	CHECK(end.position.x == doctest::Approx(radius));
	CHECK(end.position.z == doctest::Approx(-1 + radius));
	CHECK(end.orientation.y == doctest::Approx(AboutY(-kPi / 2).y));

	// A cursor whose hotspot is its upper left corner: its centre is half its size right of and below the point
	XrPosef cursor = CursorPose(At(0, 1, -2), 0.1f, 0.2f, vr::HmdVector2_t{ { 0, 0 } });
	CHECK(cursor.position.x == doctest::Approx(0.05f));
	CHECK(cursor.position.y == doctest::Approx(0.9f));
}

TEST_CASE("Quad_aspect_follows_the_texels")
{
	CHECK(QuadAspect(200, 100, 1) == doctest::Approx(2.0f));
	// Texels twice as wide as tall make the quad twice as wide for the same texture
	CHECK(QuadAspect(200, 100, 2) == doctest::Approx(4.0f));
	CHECK(QuadAspect(200, -100, 0.5f) == doctest::Approx(1.0f));
	// Nothing sensible: square
	CHECK(QuadAspect(0, 100, 1) == 1.0f);
	CHECK(QuadAspect(100, 0, 1) == 1.0f);
	CHECK(QuadAspect(200, 100, 0) == doctest::Approx(2.0f));
	CHECK(QuadAspect(200, 100, -3) == doctest::Approx(2.0f));
	CHECK(QuadAspect(200, 100, std::nanf("")) == doctest::Approx(2.0f));
}

TEST_CASE("Colour_scale_is_the_overlay_colour_and_alpha")
{
	XrColor4f c = ColourScale(0.5f, 0.25f, 1, 0.5f, false);
	CHECK(c.r == doctest::Approx(0.5f));
	CHECK(c.g == doctest::Approx(0.25f));
	CHECK(c.b == doctest::Approx(1));
	CHECK(c.a == doctest::Approx(0.5f));

	// A premultiplied texture fades its colour with its alpha
	c = ColourScale(1, 1, 1, 0.5f, true);
	CHECK(c.r == doctest::Approx(0.5f));
	CHECK(c.a == doctest::Approx(0.5f));

	// Kept in range
	c = ColourScale(2, -1, std::nanf(""), 3, false);
	CHECK(c.r == 1);
	CHECK(c.g == 0);
	CHECK(c.b == 0);
	CHECK(c.a == 1);
}

TEST_CASE("Subtended_angle_of_a_quad")
{
	CHECK(SubtendedAngle(2, 1) == doctest::Approx(kPi / 2));
	CHECK(SubtendedAngle(0.01f, 5) < 0.01f);
	CHECK(SubtendedAngle(1, 0) == doctest::Approx(kPi));
	CHECK(SubtendedAngle(0, 1) == 0);
	CHECK(SubtendedAngle(1, -1) == 0);
}

TEST_CASE("Draw_order_puts_the_high_quality_overlay_on_top_then_sort_order_then_the_nearest")
{
	const DrawOrder farther{ false, 0, 3 }, nearer{ false, 0, 1 }, sorted{ false, 2, 5 }, high{ true, 0, 9 };
	CHECK(DrawnBefore(farther, nearer));
	CHECK_FALSE(DrawnBefore(nearer, farther));
	CHECK(DrawnBefore(nearer, sorted));
	CHECK_FALSE(DrawnBefore(sorted, nearer));
	CHECK(DrawnBefore(sorted, high));
	CHECK_FALSE(DrawnBefore(high, sorted));
	CHECK_FALSE(DrawnBefore(high, high));
}

TEST_CASE("Image_rectangle_selects_the_area_of_a_copy_and_follows_its_mirroring")
{
	XrRect2Di r = ImageRect(10, 20, 50, 60, 100, 200, false, false);
	CHECK(r.offset.x == 10);
	CHECK(r.offset.y == 20);
	CHECK(r.extent.width == 40);
	CHECK(r.extent.height == 40);

	r = ImageRect(10, 20, 50, 60, 100, 200, true, false);
	CHECK(r.offset.x == 50);
	CHECK(r.offset.y == 20);
	CHECK(r.extent.width == 40);

	r = ImageRect(10, 20, 50, 60, 100, 200, false, true);
	CHECK(r.offset.x == 10);
	CHECK(r.offset.y == 140);
	CHECK(r.extent.height == 40);

	r = ImageRect(0, 0, 100, 200, 100, 200, true, true);
	CHECK(r.offset.x == 0);
	CHECK(r.offset.y == 0);
	CHECK(r.extent.width == 100);
	CHECK(r.extent.height == 200);
}
