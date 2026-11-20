// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

#include "Reimpl/BaseNotifications.h"

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
