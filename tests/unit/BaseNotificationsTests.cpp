// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

#include "Reimpl/BaseNotifications.h"

#include <vector>

TEST_CASE("Notification_ids_are_kept_until_removed")
{
	BaseNotifications n;
	uint32_t a = 0, b = 0;
	REQUIRE(n.Add(0, 0, &a) == vr::VRNotificationError_OK);
	REQUIRE(n.Add(1, 0, &b) == vr::VRNotificationError_OK);
	CHECK(a != 0);
	CHECK(b != 0);
	CHECK(a != b);

	CHECK(n.RemoveNotification(a) == vr::VRNotificationError_OK);
	CHECK(n.RemoveNotification(a) == vr::VRNotificationError_InvalidNotificationId);
	CHECK(n.RemoveNotification(b) == vr::VRNotificationError_OK);
}

TEST_CASE("Removing_unknown_notification_ids")
{
	BaseNotifications n;
	// The invalid id is accepted, any other that isn't showing is not
	CHECK(n.RemoveNotification(0) == vr::VRNotificationError_OK);
	CHECK(n.RemoveNotification(0xffffffffu) == vr::VRNotificationError_InvalidNotificationId);
	CHECK(n.RemoveNotification(1) == vr::VRNotificationError_InvalidNotificationId);
}

TEST_CASE("System_notification_with_the_same_user_value_is_refused")
{
	BaseNotifications n;
	uint32_t a = 0, b = 0, c = 0;
	REQUIRE(n.Add(2, 7, &a) == vr::VRNotificationError_OK);
	CHECK(n.Add(2, 7, &b) == vr::VRNotificationError_SystemWithUserValueAlreadyExists);
	CHECK(n.Add(2, 8, &c) == vr::VRNotificationError_OK);
	n.RemoveNotification(a);
	CHECK(n.Add(2, 7, &b) == vr::VRNotificationError_OK);
}

TEST_CASE("Notification_for_an_unknown_overlay_is_refused")
{
	BaseNotifications n;
	uint32_t id = 99;
	CHECK(n.CreateNotification(1, 0, 0, "text", 0, nullptr, &id) == vr::VRNotificationError_InvalidOverlayHandle);
	CHECK(id == 0);
	CHECK(n.CreateNotification(vr::k_ulOverlayHandleInvalid, 0, 0, "text", 0, nullptr, nullptr) == vr::VRNotificationError_InvalidOverlayHandle);
}

namespace {
std::vector<uint32_t> Types(const std::vector<BaseNotifications::Event>& events)
{
	std::vector<uint32_t> out;
	for (const BaseNotifications::Event& e : events)
		out.push_back(e.event.eventType);
	return out;
}
} // namespace

TEST_CASE("Notifications_are_shown_one_at_a_time_with_their_events")
{
	BaseNotifications n;
	uint32_t a = 0, b = 0;
	REQUIRE(n.Add(0, 11, &a, 5, "first") == vr::VRNotificationError_OK);
	REQUIRE(n.Add(1, 22, &b, 6, "second") == vr::VRNotificationError_OK); // persistent
	std::vector<BaseNotifications::Event> events;

	n.Update(100.0, events);
	REQUIRE(events.size() == 1);
	CHECK(events[0].event.eventType == vr::VREvent_Notification_Shown);
	CHECK(events[0].overlay == 5);
	CHECK(events[0].event.data.notification.notificationId == a);
	CHECK(events[0].event.data.notification.ulUserValue == 11);
	REQUIRE(n.Current());
	CHECK(n.Current()->text == "first");

	// Its time over: hidden and gone (transient), and the next shown
	events.clear();
	n.Update(100.0 + BaseNotifications::kShownSeconds, events);
	CHECK(Types(events) == std::vector<uint32_t>{ vr::VREvent_Notification_Hidden, vr::VREvent_Notification_Shown });
	CHECK(events[1].overlay == 6);
	CHECK(n.Current()->id == b);
	CHECK(n.RemoveNotification(a) == vr::VRNotificationError_InvalidNotificationId);

	// A persistent one is hidden after its time, and stays until removed
	events.clear();
	n.Update(200.0, events);
	CHECK(Types(events) == std::vector<uint32_t>{ vr::VREvent_Notification_Hidden });
	CHECK_FALSE(n.Current());
	events.clear();
	CHECK(n.RemoveNotification(b) == vr::VRNotificationError_OK);
	n.Update(201.0, events);
	CHECK(Types(events) == std::vector<uint32_t>{ vr::VREvent_Notification_Destroyed });
	CHECK(events[0].event.data.notification.ulUserValue == 22);
}

TEST_CASE("A_notification_removed_while_shown_is_hidden_then_destroyed")
{
	BaseNotifications n;
	uint32_t a = 0, b = 0;
	REQUIRE(n.Add(0, 0, &a, 5, "first") == vr::VRNotificationError_OK);
	REQUIRE(n.Add(0, 0, &b, 5, "second") == vr::VRNotificationError_OK);
	std::vector<BaseNotifications::Event> events;
	n.Update(1.0, events);
	events.clear();
	CHECK(n.RemoveNotification(a) == vr::VRNotificationError_OK);
	n.Update(1.5, events);
	// Hidden and gone, and the next one shown at once
	CHECK(Types(events) == std::vector<uint32_t>{ vr::VREvent_Notification_Hidden, vr::VREvent_Notification_Destroyed, vr::VREvent_Notification_Shown });
	CHECK(n.Current()->id == b);
}

TEST_CASE("Notification_events_never_taken_are_kept_to_a_bound")
{
	BaseNotifications n;
	for (int i = 0; i < 1000; i++) {
		uint32_t id = 0;
		REQUIRE(n.Add(0, 0, &id, 5, "x") == vr::VRNotificationError_OK);
		REQUIRE(n.RemoveNotification(id) == vr::VRNotificationError_OK);
	}
	std::vector<BaseNotifications::Event> events;
	n.Update(1.0, events);
	CHECK(events.size() <= 256);
	CHECK_FALSE(events.empty());
}
