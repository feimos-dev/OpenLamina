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

	// A hold: a recentre from that hand once it has been held long enough, once, and no dashboard when it's let go
	CHECK(d.OnInput(false, true, 2.0) == none);
	CHECK(d.OnInput(false, true, 2.0 + Dashboard::kRecentreHold - 0.01) == none);
	CHECK(d.OnInput(false, true, 2.0 + Dashboard::kRecentreHold) == 1);
	CHECK(d.OnInput(false, true, 3.5) == none);
	CHECK(d.OnInput(false, false, 3.6) == none);
	CHECK(!d.Open());
	// The next press is a press again
	d.OnInput(true, false, 7.0);
	d.OnInput(false, false, 7.1);
	CHECK(d.Open());

	// A hold while it's open recentres and leaves it open
	CHECK(d.OnInput(true, false, 8.0) == none);
	CHECK(d.OnInput(true, false, 9.5) == 0);
	CHECK(d.OnInput(false, false, 9.6) == none);
	CHECK(d.Open());
}

TEST_CASE("Long_hold_recentres_once")
{
	const int none = Dashboard::kNoHand;
	Dashboard d(Dashboard::Button::PressOrHold);
	// Held on (turning the controller off): the one recentre at the second, nothing after, held or let go
	CHECK(d.OnInput(false, true, 0.0) == none);
	CHECK(d.OnInput(false, true, 1.5) == 1);
	CHECK(d.OnInput(false, true, 3.0) == none);
	CHECK(d.OnInput(false, true, 5.0) == none);
	CHECK(d.OnInput(false, false, 5.1) == none);
	CHECK(!d.Open());
	// No input while held (the controller gone), let go after the second: neither a recentre nor the dashboard
	CHECK(d.OnInput(true, false, 6.0) == none);
	CHECK(d.OnInput(false, false, 6.0 + Dashboard::kRecentreHold + 0.5) == none);
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
	CHECK(d.OnInput(true, true, 1.2) == 0);
	CHECK(d.OnInput(false, false, 1.5) == none);
	CHECK(!d.Open());
	// One let go, the other still down: the same press, the other hand's once the second is up
	CHECK(d.OnInput(true, true, 2.0) == none);
	CHECK(d.OnInput(false, true, 2.5) == none);
	CHECK(d.OnInput(false, true, 3.2) == 1);
	CHECK(d.OnInput(false, false, 3.3) == none);
	CHECK(!d.Open());

	// Begun on the left, which is let go while the right holds on: the right hand's, timed from the start
	CHECK(d.OnInput(true, false, 4.0) == none);
	CHECK(d.OnInput(true, true, 4.3) == none);
	CHECK(d.OnInput(false, true, 4.6) == none);
	CHECK(!d.Open());
	CHECK(d.OnInput(false, true, 5.0) == 1);
	CHECK(d.OnInput(false, false, 5.1) == none);
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

TEST_CASE("A_tab_with_no_room_is_neither_drawn_nor_clicked")
{
	Dashboard d(Dashboard::Button::MenuLong);
	d.isAlive = [](uint32_t, uint64_t) { return true; };
	Json::Value doc(Json::objectValue);
	for (uint32_t pid = 300; pid < 310; pid++)
		doc[std::to_string(pid)] = OverlaysWithDashboard(pid, 77)[std::to_string(pid)];
	Canvas canvas(Dashboard::kWidth, Dashboard::kHeight);
	d.Draw(canvas, Json::Value(Json::objectValue), doc);
	// The first tab past the frame's right margin (20 pixels) is the first not drawn
	size_t fit = 0;
	while (fit < 10 && Dashboard::TabRect(fit).x + Dashboard::TabRect(fit).w <= Dashboard::kWidth - 20)
		fit++;
	REQUIRE(fit < 10);
	// The first tab that doesn't fit, where it is on the canvas
	const Dashboard::Rect r = Dashboard::TabRect(fit);
	REQUIRE(r.x + 1 < Dashboard::kWidth);
	const float x = (float)r.x + 1, y = (float)r.y + r.h / 2.0f;
	std::optional<std::string> clicked = d.OnClick(x / Dashboard::kWidth, y / Dashboard::kHeight);
	CHECK_FALSE((clicked && *clicked == "tab:" + std::to_string(300 + fit) + ":77"));
}

TEST_CASE("Dashboard_tabs_of_a_document_of_another_shape")
{
	Dashboard d(Dashboard::Button::MenuLong);
	d.isAlive = [](uint32_t, uint64_t) { return true; };
	Json::Value doc = OverlaysWithDashboard(222, 77);
	doc["222"]["overlays"][1]["thumbnail"] = "yes";
	doc["222"]["app"] = 3;
	doc["223"] = "an entry";
	doc["224"]["overlays"] = 1;
	std::vector<Dashboard::Tab> tabs;
	REQUIRE_NOTHROW(tabs = d.Tabs(doc));
	// The thumbnail whose flag can't be read is taken for a main overlay
	CHECK(tabs.size() == 2);
	Canvas canvas(Dashboard::kWidth, Dashboard::kHeight);
	REQUIRE_NOTHROW(d.Draw(canvas, Json::Value(Json::objectValue), doc));
}

TEST_CASE("A_tab_label_is_kept_short_and_its_characters_whole")
{
	Dashboard d(Dashboard::Button::MenuLong);
	d.isAlive = [](uint32_t, uint64_t) { return true; };
	Json::Value doc = OverlaysWithDashboard(222, 77);
	// A long name is cut; a character beyond 16 bits leaves the rest of its label as it is
	doc["222"]["overlays"][0]["name"] = std::string(100000, 'x');
	doc["222"]["overlays"][1]["thumbnail"] = false;
	doc["222"]["overlays"][1]["name"] = "\xC3\xA9t\xC3\xA9 \xF0\x9F\x98\x80";
	std::vector<Dashboard::Tab> tabs = d.Tabs(doc);
	REQUIRE(tabs.size() == 2);
	CHECK(tabs[0].label.size() == 256);
	CHECK(tabs[1].label.substr(0, 4) == L"\xE9t\xE9 ");
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

TEST_CASE("Dashboard_document_names_a_running_host")
{
	auto alive = [](uint32_t pid, uint64_t start) { return pid == 111 && start == 5; };
	Json::Value doc(Json::objectValue);
	CHECK(!DashboardDocHosted(doc, alive));
	CHECK(!DashboardDocHosted(Json::Value(), alive));

	doc["host_pid"] = 111;
	doc["host_start"] = (Json::UInt64)5;
	CHECK(DashboardDocHosted(doc, alive));
	// Whatever a scene app left in it
	doc["open"] = true;
	doc["scene_pid"] = 222;
	CHECK(DashboardDocHosted(doc, alive));
	CHECK(!DashboardDocOpen(doc, alive));

	// A host that has ended, or whose pid now belongs to another process: none
	Json::Value gone = doc;
	gone["host_pid"] = 222;
	CHECK(!DashboardDocHosted(gone, alive));
	gone = doc;
	gone["host_start"] = (Json::UInt64)6;
	CHECK(!DashboardDocHosted(gone, alive));
	gone = doc;
	gone["host_pid"] = "111";
	CHECK(!DashboardDocHosted(gone, alive));
}

TEST_CASE("Dashboard_host_quit_request_is_live_for_two_seconds")
{
	auto alive = [](uint32_t pid, uint64_t start) { return pid == 111 && start == 5; };
	Json::Value doc(Json::objectValue);
	doc["host_pid"] = 111;
	doc["host_start"] = (Json::UInt64)5;
	CHECK(DashboardDocQuit(doc, 100, alive) == 0);

	doc["quit_at"] = 100.0;
	CHECK(DashboardDocQuit(doc, 100, alive) == 100.0);
	CHECK(DashboardDocQuit(doc, 101.5, alive) == 100.0);
	// Too old, or from the future
	CHECK(DashboardDocQuit(doc, 102, alive) == 0);
	CHECK(DashboardDocQuit(doc, 99, alive) == 0);
	// Not a number, or not a running host's
	doc["quit_at"] = "100";
	CHECK(DashboardDocQuit(doc, 100, alive) == 0);
	doc["quit_at"] = 100.0;
	doc["host_pid"] = 222;
	CHECK(DashboardDocQuit(doc, 100, alive) == 0);
}

TEST_CASE("Dashboard_gives_way_closed_and_drops_the_press_under_way")
{
	Dashboard d(Dashboard::Button::PressOrHold);
	// Open, then a press begun
	d.OnInput(true, false, 1.0);
	d.OnInput(false, false, 1.1);
	REQUIRE(d.Open());
	d.OnInput(false, true, 2.0);

	d.GiveWay();
	CHECK(!d.Open());
	// Letting go of that press opens nothing, and holding it on recentres nothing
	CHECK(d.OnInput(false, true, 2.5) == Dashboard::kNoHand);
	CHECK(d.OnInput(false, true, 4.0) == Dashboard::kNoHand);
	CHECK(d.OnInput(false, false, 4.1) == Dashboard::kNoHand);
	CHECK(!d.Open());
	// The next press is this one's again
	d.OnInput(true, false, 5.0);
	d.OnInput(false, false, 5.1);
	CHECK(d.Open());
}

TEST_CASE("Dashboard_primary_hand_is_the_laser_on_it_else_the_last_driver")
{
	CHECK(Dashboard::PrimaryHand(0, 1) == 0);
	CHECK(Dashboard::PrimaryHand(1, 0) == 1);
	CHECK(Dashboard::PrimaryHand(-1, 0) == 0);
	CHECK(Dashboard::PrimaryHand(-1, 1) == 1);
}
