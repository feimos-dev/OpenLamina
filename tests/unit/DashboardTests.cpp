// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

#include "Misc/Dashboard.h"

namespace {

// The Overlays document with one program's dashboard overlay (and its thumbnail, which gets no tab)
Json::Value OverlaysWithDashboard(uint32_t pid, uint64_t handle)
{
	Json::Value main;
	main["handle"] = (Json::UInt64)handle;
	main["name"] = "Tool";
	main["dashboard"] = true;
	Json::Value thumb;
	thumb["handle"] = (Json::UInt64)(handle + 1);
	thumb["name"] = "Tool";
	thumb["dashboard"] = true;
	thumb["thumbnail"] = true;
	Json::Value doc;
	Json::Value& entry = doc[std::to_string(pid)];
	entry["app"] = "test.tool";
	entry["overlays"].append(main);
	entry["overlays"].append(thumb);
	return doc;
}

std::pair<float, float> Centre(const Dashboard::Rect& r)
{
	return { (r.x + r.w / 2.0f) / Dashboard::kWidth, (r.y + r.h / 2.0f) / Dashboard::kHeight };
}

} // namespace

TEST_CASE("Long_press_opens_short_does_not")
{
	Dashboard d(Dashboard::Button::MenuLong);
	d.OnInput(true, 0.0);
	d.OnInput(true, 0.3);
	d.OnInput(false, 0.35);
	CHECK(!d.Open());
	d.OnInput(true, 1.0);
	d.OnInput(true, 1.5);
	CHECK(!d.Open());
	d.OnInput(true, 1.7);
	CHECK(d.Open());
	// Held on, it doesn't close again; a second long press does
	d.OnInput(true, 3.0);
	CHECK(d.Open());
	d.OnInput(false, 3.1);
	d.OnInput(true, 4.0);
	d.OnInput(true, 4.7);
	CHECK(!d.Open());

	Dashboard menu(Dashboard::Button::Menu);
	menu.OnInput(true, 0.0);
	CHECK(menu.Open());
	menu.OnInput(false, 0.1);
	menu.OnInput(true, 0.2);
	CHECK(!menu.Open());

	Dashboard off(Dashboard::Button::Off);
	off.OnInput(true, 0.0);
	off.OnInput(true, 2.0);
	CHECK(!off.Open());
}

TEST_CASE("Press_toggles_on_release_and_hold_recentres")
{
	const int none = Dashboard::kNoHand;
	Dashboard d(Dashboard::Button::PressOrHold);
	// A short press of the left button: nothing while it's down, open when it's let go
	CHECK(d.OnInput(true, false, 0.0) == none);
	CHECK(d.OnInput(true, false, 0.5) == none);
	CHECK(!d.Open());
	CHECK(d.OnInput(false, false, 0.6) == none);
	CHECK(d.Open());
	// And of the right one: closed again
	CHECK(d.OnInput(false, true, 1.0) == none);
	CHECK(d.Open());
	CHECK(d.OnInput(false, false, 1.2) == none);
	CHECK(!d.Open());

	// A hold: a recentre from that hand when it's let go after being held long enough, and no dashboard
	CHECK(d.OnInput(false, true, 2.0) == none);
	CHECK(d.OnInput(false, true, 2.0 + Dashboard::kRecentreHold) == none);
	CHECK(d.OnInput(false, true, 3.5) == none);
	CHECK(d.OnInput(false, false, 3.6) == 1);
	CHECK(!d.Open());
	// The next press is a press again
	d.OnInput(true, false, 7.0);
	d.OnInput(false, false, 7.1);
	CHECK(d.Open());

	// A hold while it's open recentres and leaves it open
	CHECK(d.OnInput(true, false, 8.0) == none);
	CHECK(d.OnInput(true, false, 9.5) == none);
	CHECK(d.OnInput(false, false, 9.6) == 0);
	CHECK(d.Open());
}

TEST_CASE("Hold_to_turn_the_controller_off_doesnt_recentre")
{
	const int none = Dashboard::kNoHand;
	Dashboard d(Dashboard::Button::PressOrHold);
	// Held past kPowerOffHold: neither a recentre nor the dashboard, held or let go
	CHECK(d.OnInput(false, true, 0.0) == none);
	CHECK(d.OnInput(false, true, 1.5) == none);
	CHECK(d.OnInput(false, true, Dashboard::kPowerOffHold) == none);
	CHECK(d.OnInput(false, true, 5.0) == none);
	CHECK(d.OnInput(false, false, 5.1) == none);
	CHECK(!d.Open());
	// The controller gone while held (no button down from it): the same
	CHECK(d.OnInput(true, false, 6.0) == none);
	CHECK(d.OnInput(false, false, 6.0 + Dashboard::kPowerOffHold + 0.5) == none);
	CHECK(!d.Open());
	// The next press is a press again
	d.OnInput(true, false, 10.0);
	d.OnInput(false, false, 10.1);
	CHECK(d.Open());
}

TEST_CASE("Hold_with_both_hands")
{
	const int none = Dashboard::kNoHand;
	Dashboard d(Dashboard::Button::PressOrHold);
	// Both buttons down are one press, from the hand that began it
	CHECK(d.OnInput(true, false, 0.0) == none);
	CHECK(d.OnInput(true, true, 0.5) == none);
	CHECK(d.OnInput(true, true, 1.2) == none);
	CHECK(d.OnInput(false, false, 1.5) == 0);
	CHECK(!d.Open());
	// One let go, the other still down: the same press, the other hand's when it's let go
	CHECK(d.OnInput(true, true, 2.0) == none);
	CHECK(d.OnInput(false, true, 2.5) == none);
	CHECK(d.OnInput(false, true, 3.2) == none);
	CHECK(d.OnInput(false, false, 3.3) == 1);
	CHECK(!d.Open());

	// Begun on the left, which is let go while the right holds on: the right hand's, timed from the start
	CHECK(d.OnInput(true, false, 4.0) == none);
	CHECK(d.OnInput(true, true, 4.3) == none);
	CHECK(d.OnInput(false, true, 4.6) == none);
	CHECK(!d.Open());
	CHECK(d.OnInput(false, true, 5.0) == none);
	CHECK(d.OnInput(false, false, 5.1) == 1);
	CHECK(!d.Open());

	// Both pressed and let go shortly: one press
	d.OnInput(true, true, 6.0);
	d.OnInput(true, false, 6.1);
	CHECK(!d.Open());
	d.OnInput(false, false, 6.2);
	CHECK(d.Open());
}

TEST_CASE("Other_buttons_never_recentre")
{
	for (Dashboard::Button button : { Dashboard::Button::Menu, Dashboard::Button::MenuLong, Dashboard::Button::Off }) {
		Dashboard d(button);
		bool recentred = false;
		for (int i = 0; i <= 30; i++)
			recentred = recentred || d.OnInput(true, true, i * 0.1) != Dashboard::kNoHand;
		recentred = recentred || d.OnInput(false, false, 3.1) != Dashboard::kNoHand;
		CHECK(!recentred);
		// As before: at the press, after 0.6 s, never
		CHECK(d.Open() == (button != Dashboard::Button::Off));
	}
}

TEST_CASE("Tab_click_selects_program")
{
	Dashboard d(Dashboard::Button::MenuLong);
	d.isAlive = [](uint32_t, uint64_t) { return true; };
	Canvas canvas(Dashboard::kWidth, Dashboard::kHeight);
	d.Draw(canvas, Json::Value(Json::objectValue), OverlaysWithDashboard(222, 77));
	auto [u, v] = Centre(Dashboard::TabRect(0));
	std::optional<std::string> clicked = d.OnClick(u, v);
	REQUIRE(clicked);
	CHECK(*clicked == "tab:222:77");
	// One tab: the thumbnail has none, and there's nothing where a second would be
	auto [u2, v2] = Centre(Dashboard::TabRect(1));
	CHECK(!d.OnClick(u2, v2));
	// A program that has ended has no tab
	d.isAlive = [](uint32_t, uint64_t) { return false; };
	d.Draw(canvas, Json::Value(Json::objectValue), OverlaysWithDashboard(222, 77));
	CHECK(!d.OnClick(u, v));
}

TEST_CASE("Quit_click_returns_quit")
{
	Dashboard d(Dashboard::Button::MenuLong);
	d.isAlive = [](uint32_t, uint64_t) { return true; };
	Canvas canvas(Dashboard::kWidth, Dashboard::kHeight);
	d.Draw(canvas, Json::Value(Json::objectValue), Json::Value(Json::objectValue));
	auto [u, v] = Centre(Dashboard::QuitRect());
	std::optional<std::string> clicked = d.OnClick(u, v);
	REQUIRE(clicked);
	CHECK(*clicked == "quit");
	// The panel in the middle is the program's: no click of the dashboard's there
	CHECK(!d.OnClick(0.5f, 0.5f));
}

TEST_CASE("Dashboard_document_stale_and_live")
{
	// 111 runs (start 5); 222 ran once
	ProcessAlive alive = [](uint32_t pid, uint64_t start) { return pid == 111 && start == 5; };
	Json::Value open;
	open["open"] = true;
	open["scene_pid"] = 111;
	open["scene_start"] = (Json::UInt64)5;
	CHECK(DashboardDocOpen(open, alive));
	CHECK(!DashboardDocRequest(open, 100, alive)); // a scene app's document is never a request

	// A scene app that crashed with it open, or whose pid now belongs to another process: closed
	Json::Value stale = open;
	stale["scene_pid"] = 222;
	CHECK(!DashboardDocOpen(stale, alive));
	stale = open;
	stale["scene_start"] = (Json::UInt64)6;
	CHECK(!DashboardDocOpen(stale, alive));

	Json::Value request;
	request["open"] = true;
	request["requested_by"] = 111;
	request["requested_start"] = (Json::UInt64)5;
	request["requested_at"] = 100.0;
	CHECK(DashboardDocRequest(request, 101, alive));
	CHECK(!DashboardDocOpen(request, alive));
	// Too old, from the future, or from a program that has ended
	CHECK(!DashboardDocRequest(request, 103, alive));
	CHECK(!DashboardDocRequest(request, 99, alive));
	request["requested_by"] = 222;
	CHECK(!DashboardDocRequest(request, 101, alive));
}

TEST_CASE("Dashboard_primary_hand_is_the_laser_on_it_else_the_last_driver")
{
	CHECK(Dashboard::PrimaryHand(0, 1) == 0);
	CHECK(Dashboard::PrimaryHand(1, 0) == 1);
	CHECK(Dashboard::PrimaryHand(-1, 0) == 0);
	CHECK(Dashboard::PrimaryHand(-1, 1) == 1);
}
