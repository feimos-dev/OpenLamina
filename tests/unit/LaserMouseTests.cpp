// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

#include "Misc/LaserMouse.h"

#include <vector>

using namespace overlay_maths;

namespace {

// A 1 m square overlay facing +Z, its centre at (x, y, z), 100 x 100 in mouse units
PointerTarget Square(vr::VROverlayHandle_t handle, float x, float y, float z, int sortOrder = 0, uint32_t flags = 0)
{
	PointerTarget t{};
	t.handle = handle;
	t.placement = Placement{ XrPosef{ { 0, 0, 0, 1 }, { x, y, z } }, 1, 1, 0 };
	t.mouseScale = { { 100, 100 } };
	t.flags = flags;
	t.sortOrder = sortOrder;
	return t;
}

// A hand's laser from (x, y, 0) along -Z
PointerRay Ray(int hand, float x, float y, bool click = false, float scrollY = 0)
{
	PointerRay r{};
	r.hand = hand;
	r.origin = { x, y, 0 };
	r.dir = { 0, 0, -1 };
	r.click = click;
	r.scrollY = scrollY;
	r.valid = true;
	r.device = hand == 0 ? 1 : 2;
	return r;
}

PointerRay None(int hand)
{
	PointerRay r{};
	r.hand = hand;
	return r;
}

// The events of these types, in order
std::vector<PointerEvent> Only(const std::vector<PointerEvent>& events, std::initializer_list<uint32_t> types)
{
	std::vector<PointerEvent> out;
	for (const PointerEvent& e : events)
		for (uint32_t t : types)
			if (e.event.eventType == t)
				out.push_back(e);
	return out;
}

const std::initializer_list<uint32_t> kMouse = { vr::VREvent_MouseMove, vr::VREvent_MouseButtonDown, vr::VREvent_MouseButtonUp };

} // namespace

TEST_CASE("Move_then_click_then_release")
{
	LaserMouse mouse;
	std::vector<PointerTarget> targets = { Square(7, 0, 0, -2) };
	std::vector<PointerEvent> events;
	// A quarter in from the left and up from the bottom: (25, 25) in mouse units, which count from the lower left
	mouse.Update({ None(0), Ray(1, -0.25f, -0.25f) }, targets, 0.0, events);
	mouse.Update({ None(0), Ray(1, -0.25f, -0.25f, true) }, targets, 0.1, events);
	mouse.Update({ None(0), Ray(1, -0.25f, -0.25f, false) }, targets, 0.2, events);

	std::vector<PointerEvent> m = Only(events, kMouse);
	REQUIRE(m.size() == 3);
	CHECK(m[0].event.eventType == vr::VREvent_MouseMove);
	CHECK(m[1].event.eventType == vr::VREvent_MouseButtonDown);
	CHECK(m[2].event.eventType == vr::VREvent_MouseButtonUp);
	for (const PointerEvent& e : m) {
		CHECK(e.handle == 7);
		CHECK(e.event.trackedDeviceIndex == 2);
		CHECK(e.event.data.mouse.x == doctest::Approx(25.0f));
		CHECK(e.event.data.mouse.y == doctest::Approx(25.0f));
	}
	CHECK(m[1].event.data.mouse.button == vr::VRMouseButton_Left);
	CHECK(m[2].event.data.mouse.button == vr::VRMouseButton_Left);
	// Focus came first, on the overlay
	REQUIRE(!events.empty());
	CHECK(events[0].event.eventType == vr::VREvent_FocusEnter);
	CHECK(events[0].event.data.overlay.overlayHandle == 7);
	REQUIRE(mouse.Cursor(1).has_value());
	CHECK(mouse.Cursor(1)->u == doctest::Approx(0.25f));
	CHECK_FALSE(mouse.Cursor(0).has_value());
	CHECK(mouse.Target(1) == 7);
}

TEST_CASE("Mouse_y_counts_from_the_lower_left_unless_the_target_draws_from_the_top")
{
	// A point three quarters up the overlay: y is 75 from the bottom, 25 from the top
	for (bool yDown : { false, true }) {
		LaserMouse mouse;
		PointerTarget t = Square(7, 0, 0, -2);
		t.yDown = yDown;
		std::vector<PointerEvent> events;
		mouse.Update({ None(0), Ray(1, 0.1f, 0.25f) }, { t }, 0.0, events);
		std::vector<PointerEvent> m = Only(events, kMouse);
		REQUIRE(m.size() == 1);
		CHECK(m[0].event.data.mouse.x == doctest::Approx(60.0f));
		CHECK(m[0].event.data.mouse.y == doctest::Approx(yDown ? 25.0f : 75.0f));
	}
}

TEST_CASE("Mouse_mask_is_in_the_lower_left_coordinates")
{
	// A mask over the upper half of the overlay, in y-up units: a point near the top is in it, one near the bottom not
	PointerTarget t = Square(7, 0, 0, -2);
	OOVR_VROverlayIntersectionMaskPrimitive_t rect{};
	rect.m_nPrimitiveType = OverlayIntersectionPrimitiveType_Rectangle;
	rect.m_Primitive.m_Rectangle = { 0, 50, 100, 50 };
	t.mask.push_back(rect);
	LaserMouse mouse;
	std::vector<PointerEvent> events;
	mouse.Update({ None(0), Ray(1, 0, 0.25f) }, { t }, 0.0, events);
	CHECK(mouse.Target(1) == 7);
	mouse.Update({ None(0), Ray(1, 0, -0.25f) }, { t }, 0.1, events);
	CHECK(mouse.Target(1) == vr::k_ulOverlayHandleInvalid);
}

TEST_CASE("Nearest_overlay_wins_by_sort_order")
{
	// The farther one is sorted on top: it wins
	{
		LaserMouse mouse;
		std::vector<PointerEvent> events;
		mouse.Update({ None(0), Ray(1, 0, 0) }, { Square(1, 0, 0, -2, 0), Square(2, 0, 0, -3, 5) }, 0, events);
		CHECK(mouse.Target(1) == 2);
		std::vector<PointerEvent> m = Only(events, kMouse);
		REQUIRE(m.size() == 1);
		CHECK(m[0].handle == 2);
	}
	// With the same sort order, the nearer one wins
	{
		LaserMouse mouse;
		std::vector<PointerEvent> events;
		mouse.Update({ None(0), Ray(1, 0, 0) }, { Square(1, 0, 0, -3, 1), Square(2, 0, 0, -2, 1) }, 0, events);
		CHECK(mouse.Target(1) == 2);
		CHECK(mouse.Cursor(1)->distance == doctest::Approx(2.0f));
	}
}

TEST_CASE("Mask_excludes_hit")
{
	// The near overlay's mask is a small circle in its lower left: the ray through its centre goes on to the far one
	PointerTarget nearer = Square(1, 0, 0, -2, 0);
	OOVR_VROverlayIntersectionMaskPrimitive_t circle{};
	circle.m_nPrimitiveType = OverlayIntersectionPrimitiveType_Circle;
	circle.m_Primitive.m_Circle = { 10, 10, 5 };
	nearer.mask.push_back(circle);
	LaserMouse mouse;
	std::vector<PointerEvent> events;
	mouse.Update({ None(0), Ray(1, 0, 0) }, { nearer, Square(2, 0, 0, -3, 0) }, 0, events);
	CHECK(mouse.Target(1) == 2);
	// In the circle (10, 10 from the lower left: u 0.1, v 0.1) the near one is hit
	LaserMouse inside;
	inside.Update({ None(0), Ray(1, -0.4f, -0.4f) },{ nearer, Square(2, 0, 0, -3, 0) }, 0, events);
	CHECK(inside.Target(1) == 1);
}

TEST_CASE("Scroll_discrete_vs_smooth")
{
	auto scrolled = [](uint32_t flags) {
		LaserMouse mouse;
		std::vector<PointerEvent> events;
		std::vector<PointerTarget> targets = { Square(3, 0, 0, -2, 0, flags) };
		for (int i = 0; i <= 10; i++)
			mouse.Update({ None(0), Ray(1, 0, 0, false, 1.0f) }, targets, i * 0.011, events);
		return Only(events, { vr::VREvent_ScrollDiscrete, vr::VREvent_ScrollSmooth });
	};
	std::vector<PointerEvent> discrete = scrolled(kOverlayFlagDiscreteScroll);
	REQUIRE(!discrete.empty());
	for (const PointerEvent& e : discrete) {
		CHECK(e.event.eventType == vr::VREvent_ScrollDiscrete);
		CHECK(e.event.data.scroll.ydelta == doctest::Approx(1.0f));
	}
	// One step at once; the next only after a pause
	CHECK(discrete.size() == 1);

	std::vector<PointerEvent> smooth = scrolled(kOverlayFlagSmoothScroll);
	REQUIRE(smooth.size() >= 5);
	for (const PointerEvent& e : smooth) {
		CHECK(e.event.eventType == vr::VREvent_ScrollSmooth);
		CHECK(e.event.data.scroll.ydelta > 0.0f);
	}
	// Neither flag: no scrolling
	CHECK(scrolled(0).empty());
}

TEST_CASE("Click_owned_by_one_hand")
{
	LaserMouse mouse;
	std::vector<PointerTarget> targets = { Square(4, 0, 0, -2) };
	std::vector<PointerEvent> events;
	mouse.Update({ Ray(0, 0.1f, 0), Ray(1, -0.1f, 0) }, targets, 0.0, events);
	mouse.Update({ Ray(0, 0.1f, 0), Ray(1, -0.1f, 0, true) }, targets, 0.1, events); // right down
	mouse.Update({ Ray(0, 0.1f, 0, true), Ray(1, -0.1f, 0, true) }, targets, 0.2, events); // left too: ignored
	mouse.Update({ Ray(0, 0.1f, 0, true), Ray(1, -0.1f, 0, false) }, targets, 0.3, events); // right up
	mouse.Update({ Ray(0, 0.1f, 0, true), Ray(1, -0.1f, 0, false) }, targets, 0.4, events); // left still held: nothing
	mouse.Update({ Ray(0, 0.1f, 0, false), Ray(1, -0.1f, 0, false) }, targets, 0.5, events);
	mouse.Update({ Ray(0, 0.1f, 0, true), Ray(1, -0.1f, 0, false) }, targets, 0.6, events); // left down: its click

	std::vector<PointerEvent> clicks = Only(events, { vr::VREvent_MouseButtonDown, vr::VREvent_MouseButtonUp });
	REQUIRE(clicks.size() == 3);
	CHECK(clicks[0].event.eventType == vr::VREvent_MouseButtonDown);
	CHECK(clicks[0].event.trackedDeviceIndex == 2);
	CHECK(clicks[0].event.data.mouse.x == doctest::Approx(40.0f));
	CHECK(clicks[1].event.eventType == vr::VREvent_MouseButtonUp);
	CHECK(clicks[1].event.trackedDeviceIndex == 2);
	CHECK(clicks[2].event.eventType == vr::VREvent_MouseButtonDown);
	CHECK(clicks[2].event.trackedDeviceIndex == 1);
	CHECK(clicks[2].event.data.mouse.x == doctest::Approx(60.0f));
	CHECK(mouse.ActiveHand() == 0);
	CHECK(mouse.HandOn(4) == 0);
}

TEST_CASE("Laser_mouse_focus_leaves_when_pointing_away")
{
	LaserMouse mouse;
	std::vector<PointerTarget> targets = { Square(5, 0, 0, -2) };
	std::vector<PointerEvent> events;
	mouse.Update({ None(0), Ray(1, 0, 0) }, targets, 0, events);
	mouse.Update({ None(0), Ray(1, 3, 0) }, targets, 0.1, events);
	std::vector<PointerEvent> focus = Only(events, { vr::VREvent_FocusEnter, vr::VREvent_FocusLeave });
	REQUIRE(focus.size() == 2);
	CHECK(focus[1].event.eventType == vr::VREvent_FocusLeave);
	CHECK(focus[1].handle == 5);
	CHECK(mouse.Target(1) == vr::k_ulOverlayHandleInvalid);
	CHECK(mouse.HandOn(5) == -1);
}
