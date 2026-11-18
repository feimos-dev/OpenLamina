// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

#include "Misc/RemotePointer.h"

#include <vector>

namespace {

// A program's entry in the Overlays document with one visible mouse overlay: a 1 m square facing +Z at (0, 1.5, -2)
Json::Value Entry(uint64_t handle, int inputMethod = 1)
{
	Json::Value o;
	o["handle"] = (Json::UInt64)handle;
	o["key"] = "test.overlay";
	o["name"] = "Test";
	o["pose"]["p"] = Json::Value(Json::arrayValue);
	for (float v : { 0.0f, 1.5f, -2.0f })
		o["pose"]["p"].append(v);
	o["pose"]["q"] = Json::Value(Json::arrayValue);
	for (float v : { 0.0f, 0.0f, 0.0f, 1.0f })
		o["pose"]["q"].append(v);
	o["space"] = "standing";
	o["width"] = 1.0;
	o["height"] = 1.0;
	o["curvature"] = 0.0;
	o["input_method"] = inputMethod;
	o["flags"] = 0;
	o["sort_order"] = 0;
	o["visible"] = true;
	o["dashboard"] = false;
	Json::Value entry;
	entry["app"] = "test.app";
	entry["updated"] = 1.0;
	entry["overlays"].append(o);
	return entry;
}

// The right hand's laser from (0, 1.5, -1) along -Z, at the overlay's centre
std::array<PointerRay, 2> Rays(bool click)
{
	std::array<PointerRay, 2> rays{};
	rays[0].hand = 0;
	rays[1].hand = 1;
	rays[1].origin = { 0, 1.5f, -1 };
	rays[1].dir = { 0, 0, -1 };
	rays[1].click = click;
	rays[1].valid = true;
	rays[1].device = 2;
	return rays;
}

std::vector<RemotePointerEvent> Unpacked(const std::vector<StoreEvent>& events)
{
	std::vector<RemotePointerEvent> out;
	for (const StoreEvent& e : events) {
		REQUIRE(e.eventType == OcEvent_RemotePointer);
		out.push_back(UnpackRemotePointer(e.data));
	}
	return out;
}

bool Has(const std::vector<RemotePointerEvent>& events, uint32_t type)
{
	for (const RemotePointerEvent& e : events)
		if (e.eventType == type)
			return true;
	return false;
}

} // namespace

TEST_CASE("Remote_overlay_gets_events_for_its_pid")
{
	RemotePointer pointer;
	pointer.isAlive = [](uint32_t, uint64_t) { return true; };
	Json::Value doc;
	doc["222"] = Entry(0x1234);
	std::vector<StoreEvent> posted;
	pointer.Update(Rays(false), doc, 111, 1.0, posted);
	pointer.Update(Rays(true), doc, 111, 1.1, posted);
	pointer.Update(Rays(false), doc, 111, 1.2, posted);
	std::vector<RemotePointerEvent> events = Unpacked(posted);
	REQUIRE(!events.empty());
	for (const RemotePointerEvent& e : events) {
		CHECK(e.targetPid == 222);
		CHECK(e.handle == 0x1234);
	}
	CHECK(Has(events, vr::VREvent_MouseMove));
	CHECK(Has(events, vr::VREvent_MouseButtonDown));
	CHECK(Has(events, vr::VREvent_MouseButtonUp));
	// At the overlay's centre, in units of its width and height (the target scales them by its own mouse scale)
	for (const RemotePointerEvent& e : events) {
		if (e.eventType == vr::VREvent_MouseMove) {
			CHECK(e.x == doctest::Approx(0.5f));
			CHECK(e.y == doctest::Approx(0.5f));
			CHECK(e.device == 2);
		}
		if (e.eventType == vr::VREvent_MouseButtonDown)
			CHECK(e.button == vr::VRMouseButton_Left);
	}
}

TEST_CASE("Dead_pid_ignored")
{
	RemotePointer pointer;
	pointer.isAlive = [](uint32_t pid, uint64_t) { return pid != 222; };
	Json::Value doc;
	doc["222"] = Entry(0x1234);
	std::vector<StoreEvent> posted;
	pointer.Update(Rays(true), doc, 111, 1.0, posted);
	pointer.Update(Rays(false), doc, 111, 1.1, posted);
	CHECK(posted.empty());
	CHECK(pointer.SawDeadEntries());
}

TEST_CASE("Own_overlay_handled_locally")
{
	RemotePointer pointer;
	pointer.isAlive = [](uint32_t, uint64_t) { return true; };
	Json::Value doc;
	doc["111"] = Entry(0x1234);
	// Not a mouse overlay: nothing to point at either
	doc["333"] = Entry(0x5678, 0);
	std::vector<StoreEvent> posted;
	pointer.Update(Rays(true), doc, 111, 1.0, posted);
	pointer.Update(Rays(false), doc, 111, 1.1, posted);
	CHECK(posted.empty());
	CHECK(!pointer.SawDeadEntries());
}

TEST_CASE("Hand_relative_overlay_follows_the_hand")
{
	// A 0.3 m overlay 0.2 m ahead of the left hand (in its space), as its owner holds it
	RemotePointer pointer;
	pointer.isAlive = [](uint32_t, uint64_t) { return true; };
	Json::Value doc;
	doc["222"] = Entry(0x1234);
	Json::Value& o = doc["222"]["overlays"][0];
	o["relative_to"] = "left";
	o["pose"]["p"][0] = 0.0;
	o["pose"]["p"][1] = 0.0;
	o["pose"]["p"][2] = -0.2;
	o["width"] = 0.3;
	o["height"] = 0.3;
	// The right laser from (0, 1.5, -1) along -Z hits it with the hand at (0, 1.5, -1.3); not once the hand is 0.5 m to
	// the side, and again from there
	auto hand = [](float x) {
		return std::array<std::optional<XrPosef>, 3>{ std::nullopt, XrPosef{ { 0, 0, 0, 1 }, { x, 1.5f, -1.3f } }, std::nullopt };
	};
	std::vector<StoreEvent> posted;
	pointer.SetDevicePoses(hand(0));
	pointer.Update(Rays(false), doc, 111, 1.0, posted);
	CHECK(Has(Unpacked(posted), vr::VREvent_MouseMove));

	posted.clear();
	pointer.SetDevicePoses(hand(0.5f));
	pointer.Update(Rays(false), doc, 111, 1.1, posted);
	CHECK(Has(Unpacked(posted), vr::VREvent_FocusLeave));
	CHECK(!Has(Unpacked(posted), vr::VREvent_MouseMove));

	std::array<PointerRay, 2> moved = Rays(false);
	moved[1].origin.x = 0.5f;
	posted.clear();
	pointer.Update(moved, doc, 111, 1.2, posted);
	CHECK(Has(Unpacked(posted), vr::VREvent_FocusEnter));

	// Without the hand's pose, nothing to point at
	posted.clear();
	pointer.SetDevicePoses({});
	pointer.Update(moved, doc, 111, 1.3, posted);
	CHECK(Has(Unpacked(posted), vr::VREvent_FocusLeave));
}

TEST_CASE("Remote_overlay_mask")
{
	// Only the overlay's left half takes the pointer: the centre is at its edge, a little right of it misses
	RemotePointer pointer;
	pointer.isAlive = [](uint32_t, uint64_t) { return true; };
	Json::Value doc;
	doc["222"] = Entry(0x1234);
	Json::Value rect(Json::arrayValue);
	for (double v : { 0.0, 0.0, 0.4, 1.0 })
		rect.append(v);
	doc["222"]["overlays"][0]["mask"][0]["rect"] = rect;
	std::vector<StoreEvent> posted;
	pointer.Update(Rays(false), doc, 111, 1.0, posted);
	CHECK(!Has(Unpacked(posted), vr::VREvent_MouseMove));
	std::array<PointerRay, 2> left = Rays(false);
	left[1].origin.x = -0.3f;
	pointer.Update(left, doc, 111, 1.1, posted);
	CHECK(Has(Unpacked(posted), vr::VREvent_MouseMove));
}

TEST_CASE("Remote_pointer_event_round_trip")
{
	RemotePointerEvent e{ 4242, 0xdeadbeefcafeULL, vr::VREvent_MouseButtonDown, 0.25f, 0.75f, vr::VRMouseButton_Left, 2 };
	StoreEvent s = MakeRemotePointerEvent(e);
	RemotePointerEvent back = UnpackRemotePointer(s.data);
	CHECK(back.targetPid == e.targetPid);
	CHECK(back.handle == e.handle);
	CHECK(back.eventType == e.eventType);
	CHECK(back.x == e.x);
	CHECK(back.y == e.y);
	CHECK(back.button == e.button);
	CHECK(back.device == e.device);
}
