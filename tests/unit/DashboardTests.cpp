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
