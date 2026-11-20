#pragma once
// OpenLamina's dashboard: opened over the game with a controller's system or menu button (dashboardButton), it shows
// each program's dashboard overlay (IVROverlay::CreateDashboardOverlay) in a panel ahead of the player, with a tab
// per program to choose between them and a "Quit game" button. The scene app's instance runs it and draws the frame
// and tabs around the panel; the program whose overlay is chosen draws that overlay itself at the panel's pose (no
// texture crosses programs). The store's Dashboard document says which:
//   { "open": bool, "active_pid": pid, "active_overlay": handle, "panel_pose": {"p": [x, y, z], "q": [x, y, z, w]},
//     "scene_pid": pid, "scene_start": its start time }   (the panel's pose in the standing space)
// Another program asks the scene app to open it (ShowDashboard) with a document that names no scene app:
//   { "open": true, "requested_by": pid, "requested_start": its start time, "requested_at": wall time in seconds,
//     "requested_overlay": key }

#include "Canvas.h"

#include "json/json.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class BaseApplications;

class Dashboard {
public:
	// How its button opens it. ini dashboardButton=system|system_long|menu|menu_long|off: the button is a controller's
	// system button or the left menu button. Menu: each press opens or closes it. MenuLong: held for 0.6 s.
	// PressOrHold (system): a press opens or closes it when the button is let go, and a hold of 1 s recentres instead.
	enum class Button { MenuLong, Menu, PressOrHold, Off };

	// The frame's canvas, and its size in metres: the panel in its middle, tabs above, "Quit game" below
	static constexpr int kWidth = 1024, kHeight = 768;
	static constexpr float kWidthMetres = 1.6f, kHeightMetres = 1.2f;
	// How long the menu button is held to open or close it (MenuLong)
	static constexpr double kLongPress = 0.6;
	// How long the button is held to recentre (PressOrHold)
	static constexpr double kRecentreHold = 1.0;
	static constexpr int kNoHand = -1;

	struct Rect {
		int x, y, w, h;
		bool Contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
	};
	// Tab i along the top, and the quit button at the bottom right, in the canvas's pixels (from its top left)
	static Rect TabRect(size_t i);
	static Rect QuitRect();

	explicit Dashboard(Button button = Button::Off)
	    : button(button)
	{
	}

	// The button of each hand (0 left, 1 right) at a time in seconds: opens and closes it. Returns the hand whose button
	// has now been held long enough to recentre (once a press, and that press doesn't open or close it), else kNoHand.
	int OnInput(bool leftDown, bool rightDown, double now);
	int OnInput(bool leftDown, double now) { return OnInput(leftDown, false, now); }
	bool Open() const { return open; }
	void SetOpen(bool isOpen); // ShowDashboard, a quit, the store

	// The scene app's instance draws the frame: tabs with each program's name (from the store), and "Quit game"
	void Draw(Canvas& canvas, const Json::Value& dashboardDoc, const Json::Value& overlaysDoc) const;
	std::optional<std::string> OnClick(float u, float v); // "tab:<pid>:<handle>" or "quit"; u, v from the top left, 0 to 1

	// The hand (0 left, 1 right) that IVROverlay::GetPrimaryDashboardDevice names: the one whose laser is on the dashboard
	// (onDashboard, -1 for none), else the one that last drove the mouse (lastDriver)
	static int PrimaryHand(int onDashboard, int lastDriver) { return onDashboard >= 0 ? onDashboard : (lastDriver == 0 ? 0 : 1); }

	// The programs' dashboard overlays in the Overlays document, live ones only: each tab's id and label
	struct Tab {
		uint32_t pid;
		uint64_t handle;
		std::wstring label;
	};
	std::vector<Tab> Tabs(const Json::Value& overlaysDoc) const;

	// Whether a program (its pid and start time) is running (tests replace it)
	std::function<bool(uint32_t, uint64_t)> isAlive;

private:
	Button button;
	bool open = false;
	bool held = false; // the button of either hand, at the last OnInput
	double heldSince = 0;
	int heldHand = 0; // the hand that began the press
	bool toggledThisPress = false; // or recentred
	mutable std::vector<Tab> drawnTabs; // as last drawn, for OnClick
};

// The document's rules (alive: whether a pid with a start time runs)
using ProcessAlive = std::function<bool(uint32_t pid, uint64_t start)>;
// Open, by a scene app that's still running (a crashed one's document is a closed one)
bool DashboardDocOpen(const Json::Value& doc, const ProcessAlive& alive);
// A request a scene app adopts: no scene app named, from a program that's still running, under 2 s old at nowWall
bool DashboardDocRequest(const Json::Value& doc, double nowWall, const ProcessAlive& alive);

// Starts the installed dashboard overlay programs whose auto-launch flag is set (those already running are left as
// they are). Returns the app keys of both, the ones started and the ones already running.
std::vector<std::string> StartAutolaunchOverlays(BaseApplications& apps);
