// The helper programs' cases: overlay and background applications, alone and beside a scene app (run.py --jobs 2 with
// a shared store). Each writes one record per step, as cases.cpp's do.

#include "conformance.h"

#include "IVRChaperoneSetup_006.h"
#include "IVRCompositor_029.h"
#include "IVRInput_011.h"
#include "IVROverlay_028.h"
#include "IVRSettings_003.h"
#include "IVRSystem_022.h"

#include "gfx/GfxApi.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11.h>
#else
#include <unistd.h>
#endif

using namespace conformance;

namespace {

vr::HmdMatrix34_t At(float x, float y, float z)
{
	vr::HmdMatrix34_t m{};
	m.m[0][0] = m.m[1][1] = m.m[2][2] = 1;
	m.m[0][3] = x;
	m.m[1][3] = y;
	m.m[2][3] = z;
	return m;
}

template <typename T>
T* Interface(CaseContext& c, const char* name)
{
	vr::EVRInitError e = vr::VRInitError_None;
	return (T*)c.core->GetGenericInterface(name, &e);
}

// The dump's session, its frames and the types of the layers it kept
Json::Value DumpSummary()
{
	Json::Value r;
	Json::Value dump = ReadStandInDump();
	if (dump.isNull())
		return r;
	r["binding"] = dump["session"]["binding"];
	r["overlay"] = dump["session"]["overlay"];
	r["frame_order_errors"] = dump["frame_order_errors"];
	r["frames_ended"] = dump["frames_ended"];
	std::set<std::string> types;
	for (const Json::Value& layer : dump["layers"])
		types.insert(layer["type"].asString());
	r["layer_types"] = Json::Value(Json::arrayValue);
	for (const std::string& t : types)
		r["layer_types"].append(t);
	return r;
}

uint32_t OwnPid()
{
#ifdef _WIN32
	return (uint32_t)GetCurrentProcessId();
#else
	return (uint32_t)getpid();
#endif
}

// A document of the store the case runs with (OC_STORE_DIR), as OpenLamina wrote it: its data, null without one
Json::Value StoreDocument(const char* name)
{
	const char* dir = std::getenv("OC_STORE_DIR");
	if (!dir || !*dir)
		return Json::Value();
	std::ifstream in(std::string(dir) + "/" + name + ".json", std::ios::binary);
	Json::Value root;
	Json::CharReaderBuilder rb;
	std::string errors;
	if (!in || !Json::parseFromStream(rb, in, &root, &errors))
		return Json::Value();
	return root["data"];
}

// The last frame in the dump that has as many quads as any frame has: the frame to judge a program's overlays by.
// The frames after it have fewer, as the program destroys its overlays one by one while OpenLamina's frames go on.
uint64_t LastFrameWithEveryQuad(const Json::Value& dump)
{
	std::map<Json::UInt64, int> quads;
	for (const Json::Value& layer : dump["layers"])
		if (layer["type"].asString() == "quad")
			quads[layer["frame"].asUInt64()]++;
	int most = 0;
	for (const auto& [frame, count] : quads)
		most = std::max(most, count);
	uint64_t last = 0;
	for (const auto& [frame, count] : quads)
		if (count == most)
			last = frame;
	return last;
}

// Whether the Overlays document has an entry of another program than this one (with an overlay, if withOverlay)
bool OtherProgramInStore(bool withOverlay)
{
	Json::Value doc = StoreDocument("overlays");
	if (!doc.isObject())
		return false;
	for (const std::string& name : doc.getMemberNames())
		if (name != std::to_string(OwnPid()) && (!withOverlay || doc[name]["overlays"].size() > 0))
			return true;
	return false;
}

// Waits, at most limit, until the Overlays document has another program's overlay
bool WaitForOtherProgram(std::chrono::milliseconds limit)
{
	auto until = std::chrono::steady_clock::now() + limit;
	while (std::chrono::steady_clock::now() < until) {
		if (OtherProgramInStore(true))
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	return false;
}

std::string EventName(uint32_t type)
{
	switch (type) {
	case vr::VREvent_FocusEnter:
		return "enter";
	case vr::VREvent_FocusLeave:
		return "leave";
	case vr::VREvent_MouseMove:
		return "move";
	case vr::VREvent_MouseButtonDown:
		return "down";
	case vr::VREvent_MouseButtonUp:
		return "up";
	case vr::VREvent_OverlayShown:
		return "shown";
	case vr::VREvent_OverlayHidden:
		return "hidden";
	default:
		return std::to_string(type);
	}
}

struct SceneApp {
	vr::IVRCompositor_029::IVRCompositor* comp = nullptr;
	vr::IVRSystem_022::IVRSystem* sys = nullptr;
};

bool OpenSceneApp(CaseContext& c, SceneApp& app)
{
	if (c.core->Init(vr::VRApplication_Scene, nullptr) != vr::VRInitError_None) {
		c.Record("Init failed");
		return false;
	}
	app.comp = Interface<vr::IVRCompositor_029::IVRCompositor>(c, "IVRCompositor_029");
	app.sys = Interface<vr::IVRSystem_022::IVRSystem>(c, "IVRSystem_022");
	if (!app.comp || !app.sys) {
		c.Record("no compositor or system");
		c.core->Cleanup();
		return false;
	}
	return true;
}

// One of the game's frames, reading the old input (which loads the actions the lasers read)
void SceneFrame(SceneApp& app, const vr::Texture_t* tex)
{
	vr::TrackedDevicePose_t poses[16];
	app.comp->WaitGetPoses(poses, 16, nullptr, 0);
	vr::VRControllerState_t state{};
	app.sys->GetControllerState(2, &state, sizeof(state));
	app.comp->Submit(vr::Eye_Left, tex, nullptr, vr::Submit_Default);
	app.comp->Submit(vr::Eye_Right, tex, nullptr, vr::Submit_Default);
}

// An overlay application's mouse overlay, 1 m at (0, 1.5, -2), 200 x 100 in mouse units, from memory
vr::VROverlayHandle_t MouseOverlay(vr::IVROverlay_028::IVROverlay* ov, Json::Value& r)
{
	using namespace vr::IVROverlay_028;
	vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid;
	r["create"] = (int)ov->CreateOverlay("oc.helper.mouse", "Mouse", &h);
	std::vector<uint8_t> grey(64 * 64 * 4, 0x80);
	r["raw"] = (int)ov->SetOverlayRaw(h, grey.data(), 64, 64, 4);
	ov->SetOverlayWidthInMeters(h, 1);
	vr::HmdMatrix34_t m = At(0, 1.5f, -2);
	ov->SetOverlayTransformAbsolute(h, vr::TrackingUniverseStanding, &m);
	ov->SetOverlayInputMethod(h, VROverlayInputMethod_Mouse);
	vr::HmdVector2_t scale{ { 200, 100 } };
	ov->SetOverlayMouseScale(h, &scale);
	r["show"] = (int)ov->ShowOverlay(h);
	return h;
}

// The game's side of remote-pointer (scripts/remote_pointer.json, with laserMouse): its right laser points at the
// overlay app's overlay, 0.25 m above its centre, and clicks at frame 150 of 300, counted once the overlay app's overlay
// is in the store
void RemotePointerSceneCase(CaseContext& c)
{
	SceneApp app;
	if (!OpenSceneApp(c, app))
		return;
	Json::Value r;
	r["saw_overlay_app"] = WaitForOtherProgram(std::chrono::seconds(5));
	callers::Texture tex(ArgSet::Valid, *c.call);
	for (int frame = 1; frame <= 300; frame++)
		SceneFrame(app, tex.get());
	c.core->Cleanup();
	Json::Value dump = DumpSummary();
	r["frame_order_errors"] = dump["frame_order_errors"];
	c.Record("frames", r);
}

// The overlay app's side of remote-pointer: its mouse overlay gets the game's pointer, through the store
void RemotePointerOverlayCase(CaseContext& c)
{
	using namespace vr::IVROverlay_028;
	Json::Value r;
	r["init"] = (int)c.core->Init(vr::VRApplication_Overlay, nullptr);
	auto* ov = r["init"].asInt() == vr::VRInitError_None ? Interface<IVROverlay>(c, "IVROverlay_028") : nullptr;
	if (!ov) {
		c.Record("events", r);
		if (r["init"].asInt() == vr::VRInitError_None)
			c.core->Cleanup();
		return;
	}
	vr::VROverlayHandle_t h = MouseOverlay(ov, r);
	std::set<std::string> seen;
	int downs = 0, ups = 0;
	Json::Value down;
	// At most 10 s; done a little after the button comes up
	const auto start = std::chrono::steady_clock::now();
	auto doneAt = start + std::chrono::seconds(10);
	bool upSeen = false;
	while (std::chrono::steady_clock::now() < doneAt) {
		vr::VREvent_t ev{};
		while (ov->PollNextOverlayEvent(h, &ev, sizeof(ev))) {
			seen.insert(EventName(ev.eventType));
			downs += ev.eventType == vr::VREvent_MouseButtonDown;
			ups += ev.eventType == vr::VREvent_MouseButtonUp;
			if (ev.eventType == vr::VREvent_MouseButtonDown && down.isNull()) {
				down["x"] = std::round(ev.data.mouse.x);
				down["y"] = std::round(ev.data.mouse.y);
				down["button"] = ev.data.mouse.button;
			}
			if (ev.eventType == vr::VREvent_MouseButtonUp && !upSeen) {
				upSeen = true;
				doneAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	r["move"] = seen.count("move") > 0;
	r["down"] = down;
	r["up"] = upSeen;
	// One click, the game's alone (the overlay app doesn't point while the game does)
	r["downs"] = downs;
	r["ups"] = ups;
	ov->DestroyOverlay(h);
	c.core->Cleanup();
	Json::Value dump = DumpSummary();
	r["frame_order_errors"] = dump["frame_order_errors"];
	c.Record("events", r);
}

// The game's side of remote-pointer-peer-dies (with laserMouse): a game with IVROverlay runs 60 frames once the
// overlay app's overlay is in the store, while the overlay app ends at its 20th frame's time without shutting down.
// The game carries on, and its next write drops the ended program's entry.
void RemotePointerPeerDiesCase(CaseContext& c)
{
	SceneApp app;
	if (!OpenSceneApp(c, app))
		return;
	Json::Value r;
	r["overlay_interface"] = Interface<vr::IVROverlay_028::IVROverlay>(c, "IVROverlay_028") != nullptr;
	r["saw_overlay_app"] = WaitForOtherProgram(std::chrono::seconds(5));
	callers::Texture tex(ArgSet::Valid, *c.call);
	int frames = 0;
	for (int frame = 1; frame <= 60; frame++, frames++)
		SceneFrame(app, tex.get());
	// The entry goes at the game's next write after the overlay app has ended: a few more frames, at most 2 s
	auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (OtherProgramInStore(false) && std::chrono::steady_clock::now() < until)
		SceneFrame(app, tex.get());
	r["frames"] = frames;
	r["ended_program_left_in_store"] = OtherProgramInStore(false);
	c.core->Cleanup();
	c.Record("frames", r);
}

// The overlay app's side of remote-pointer-peer-dies: once its overlay is in the store, it ends 20 frames' time (at
// 90 Hz) later, as a crash would, without shutting OpenVR down
void RemotePointerDyingOverlayCase(CaseContext& c)
{
	using namespace vr::IVROverlay_028;
	Json::Value r;
	r["init"] = (int)c.core->Init(vr::VRApplication_Overlay, nullptr);
	auto* ov = r["init"].asInt() == vr::VRInitError_None ? Interface<IVROverlay>(c, "IVROverlay_028") : nullptr;
	if (!ov) {
		c.Record("ended", r);
		if (r["init"].asInt() == vr::VRInitError_None)
			c.core->Cleanup();
		return;
	}
	MouseOverlay(ov, r);
	auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	bool published = false;
	while (!published && std::chrono::steady_clock::now() < until) {
		Json::Value doc = StoreDocument("overlays");
		published = doc.isObject() && doc[std::to_string(OwnPid())]["overlays"].size() > 0;
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	r["published"] = published;
	std::this_thread::sleep_for(std::chrono::milliseconds(222));
	c.Record("ended", r);
#ifdef _WIN32
	TerminateProcess(GetCurrentProcess(), 0);
#else
	_exit(0);
#endif
}

// A pose from the store or the dump ({"p": [x, y, z], ...}) rounded to centimetres, null without one
Json::Value RoundedPosition(const Json::Value& pose)
{
	if (!pose["p"].isArray() || pose["p"].size() != 3)
		return Json::Value();
	Json::Value out(Json::arrayValue);
	for (const Json::Value& v : pose["p"]) {
		double r = std::round(v.asDouble() * 100) / 100;
		out.append(r == 0 ? 0.0 : r);
	}
	return out;
}

// The game's side of dashboard (scripts/dashboard.json, ini/dashboard.ini, the manifest manifest_basic.json): the
// left menu button held 0.7 s from frame 30 opens the dashboard, whose "Quit game" the right laser clicks at frame
// 150. 260 frames, at about 90 a second, once the overlay app's dashboard overlay is in the store. At frame 125 the game
// makes a dashboard overlay of its own and a plain one; only the first is published, for its tab.
void DashboardSceneCase(CaseContext& c)
{
	SceneApp app;
	if (!OpenSceneApp(c, app))
		return;
	Json::Value r;
	auto* in = Interface<vr::IVRInput_011::IVRInput>(c, "IVRInput_011");
	vr::VRActionSetHandle_t set = vr::k_ulInvalidActionSetHandle;
	vr::VRActionHandle_t trigger = vr::k_ulInvalidActionHandle;
	if (in && !c.manifest.empty()) {
		in->SetActionManifestPath(c.manifest.c_str());
		in->GetActionSetHandle("/actions/main", &set);
		in->GetActionHandle("/actions/main/in/trigger", &trigger);
	}
	r["saw_overlay_app"] = WaitForOtherProgram(std::chrono::seconds(5));
	auto* ov = Interface<vr::IVROverlay_028::IVROverlay>(c, "IVROverlay_028");
	vr::VROverlayHandle_t ownDashboard = vr::k_ulOverlayHandleInvalid, ownThumbnail = vr::k_ulOverlayHandleInvalid, ownPlain = vr::k_ulOverlayHandleInvalid;
	callers::Texture tex(ArgSet::Valid, *c.call);
	bool activated = false, deactivated = false, quit = false;
	for (int frame = 1; frame <= 260; frame++) {
		vr::TrackedDevicePose_t poses[16];
		app.comp->WaitGetPoses(poses, 16, nullptr, 0);
		if (in && set != vr::k_ulInvalidActionSetHandle) {
			vr::IVRInput_011::VRActiveActionSet_t active{};
			active.ulActionSet = set;
			in->UpdateActionState(&active, sizeof(active), 1);
		} else {
			vr::VRControllerState_t state{};
			app.sys->GetControllerState(1, &state, sizeof(state));
		}
		vr::VREvent_t ev{};
		while (app.sys->PollNextEvent(&ev, sizeof(ev))) {
			activated = activated || ev.eventType == vr::VREvent_DashboardActivated;
			deactivated = deactivated || ev.eventType == vr::VREvent_DashboardDeactivated;
			quit = quit || ev.eventType == vr::VREvent_Quit;
		}
		if (frame == 125 && ov) {
			// The game's own dashboard overlay (made once the dashboard shows the overlay app's) and a plain one
			std::vector<uint8_t> px(16 * 16 * 4, 255);
			ov->CreateDashboardOverlay("oc.test.game.dashboard", "Game", &ownDashboard, &ownThumbnail);
			ov->SetOverlayRaw(ownDashboard, px.data(), 16, 16, 4);
			ov->CreateOverlay("oc.test.game.plain", "Plain", &ownPlain);
			ov->SetOverlayRaw(ownPlain, px.data(), 16, 16, 4);
			ov->ShowOverlay(ownPlain);
		}
		if (frame == 145) {
			// The game's entry in the store has its dashboard overlay (and its thumbnail), for its dashboard's tab, and
			// not its plain overlay
			bool dashboard = false, plain = false;
			const Json::Value doc = StoreDocument("overlays");
			for (const Json::Value& o : doc[std::to_string(OwnPid())]["overlays"]) {
				dashboard = dashboard || (o["key"].asString() == "oc.test.game.dashboard" && o["dashboard"].asBool() && !o["thumbnail"].asBool());
				plain = plain || o["key"].asString() == "oc.test.game.plain";
			}
			r["own_dashboard_overlay_published"] = dashboard;
			r["own_plain_overlay_published"] = plain;
		}
		if (frame == 20 && in && trigger != vr::k_ulInvalidActionHandle) {
			// Before the menu button is pressed: the game's action is its own
			vr::IVRInput_011::InputAnalogActionData_t data{};
			in->GetAnalogActionData(trigger, &data, sizeof(data), vr::k_ulInvalidInputValueHandle);
			r["action_active_before_open"] = data.bActive;
		}
		if (frame == 120) {
			Json::Value doc = StoreDocument("dashboard");
			r["open_at_120"] = doc["open"];
			r["panel_position"] = RoundedPosition(doc["panel_pose"]);
			// The overlay app's dashboard overlay, found by its key from here and read through that handle: shown,
			// 1 m wide, at the panel
			vr::VROverlayHandle_t helpers = vr::k_ulOverlayHandleInvalid;
			if (ov && ov->FindOverlay("oc.helper.dashboard", &helpers) == vr::VROverlayError_None) {
				r["helper_overlay_visible"] = ov->IsOverlayVisible(helpers);
				float width = 0;
				ov->GetOverlayWidthInMeters(helpers, &width);
				r["helper_overlay_width"] = width;
				vr::HmdMatrix34_t centre{};
				Json::Value pose;
				if (ov->GetTransformForOverlayCoordinates(helpers, vr::TrackingUniverseStanding, { { 0.5f, 0.5f } }, &centre) == vr::VROverlayError_None)
					for (int i = 0; i < 3; i++)
						pose["p"].append(centre.m[i][3]);
				r["helper_overlay_position"] = RoundedPosition(pose);
			} else {
				r["helper_overlay_visible"] = Json::Value();
			}
			if (in && trigger != vr::k_ulInvalidActionHandle) {
				vr::IVRInput_011::InputAnalogActionData_t data{};
				in->GetAnalogActionData(trigger, &data, sizeof(data), vr::k_ulInvalidInputValueHandle);
				r["action_active_while_open"] = data.bActive;
			}
		}
		app.comp->Submit(vr::Eye_Left, tex.get(), nullptr, vr::Submit_Default);
		app.comp->Submit(vr::Eye_Right, tex.get(), nullptr, vr::Submit_Default);
		// At about the display's rate (the stand-in doesn't wait), so the overlay app has time to show the panel
		std::this_thread::sleep_for(std::chrono::milliseconds(11));
	}
	r["activated"] = activated;
	r["quit"] = quit;
	r["deactivated"] = deactivated;
	r["open_at_end"] = StoreDocument("dashboard")["open"];
	if (ov) {
		ov->DestroyOverlay(ownPlain);
		ov->DestroyOverlay(ownDashboard);
	}
	c.core->Cleanup();
	c.Record("dashboard", r);
}

// The overlay app's side of dashboard: a dashboard overlay (1 m, from memory), which it draws at the panel's pose
// while the dashboard shows it; done once the dashboard has closed again, at most 10 s
void DashboardOverlayCase(CaseContext& c)
{
	using namespace vr::IVROverlay_028;
	Json::Value r;
	r["init"] = (int)c.core->Init(vr::VRApplication_Overlay, nullptr);
	auto* ov = r["init"].asInt() == vr::VRInitError_None ? Interface<IVROverlay>(c, "IVROverlay_028") : nullptr;
	if (!ov) {
		c.Record("dashboard overlay", r);
		if (r["init"].asInt() == vr::VRInitError_None)
			c.core->Cleanup();
		return;
	}
	vr::VROverlayHandle_t main = vr::k_ulOverlayHandleInvalid, thumbnail = vr::k_ulOverlayHandleInvalid;
	r["create"] = (int)ov->CreateDashboardOverlay("oc.helper.dashboard", "Tool", &main, &thumbnail);
	r["pair"] = main != vr::k_ulOverlayHandleInvalid && thumbnail != vr::k_ulOverlayHandleInvalid && main != thumbnail;
	std::vector<uint8_t> grey(64 * 64 * 4, 0x80);
	r["raw"] = (int)ov->SetOverlayRaw(main, grey.data(), 64, 64, 4);
	ov->SetOverlayRaw(thumbnail, grey.data(), 64, 64, 4);
	ov->SetOverlayWidthInMeters(main, 1);
	bool sawOpen = false, active = false, visible = false, atPanelWhileShown = false;
	// The overlay's own events, in order, a repeated one once: shown when the dashboard shows it, hidden when it closes
	Json::Value events(Json::arrayValue);
	// The game's dashboard overlay (made at its frame 125), found by its key and read through that handle
	vr::VROverlayHandle_t games = vr::k_ulOverlayHandleInvalid;
	std::string gamesKey;
	Json::Value panel;
	auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	auto closedAt = std::chrono::steady_clock::time_point::max();
	while (std::chrono::steady_clock::now() < until && std::chrono::steady_clock::now() < closedAt) {
		Json::Value doc = StoreDocument("dashboard");
		const bool open = doc["open"].isBool() && doc["open"].asBool();
		if (open && doc["active_pid"].asUInt() == OwnPid()) {
			sawOpen = true;
			panel = doc["panel_pose"];
			active = active || ov->IsActiveDashboardOverlay(main);
			visible = visible || ov->IsOverlayVisible(main);
			// Where the dashboard shows it: its centre at the panel's
			vr::HmdMatrix34_t centre{};
			if (ov->GetTransformForOverlayCoordinates(main, vr::TrackingUniverseStanding, { { 0.5f, 0.5f } }, &centre) == vr::VROverlayError_None) {
				Json::Value pose;
				for (int i = 0; i < 3; i++)
					pose["p"].append(centre.m[i][3]);
				atPanelWhileShown = atPanelWhileShown || RoundedPosition(pose) == RoundedPosition(panel);
			}
		}
		vr::VREvent_t ev{};
		while (ov->PollNextOverlayEvent(main, &ev, sizeof(ev))) {
			const std::string name = EventName(ev.eventType);
			if ((name == "shown" || name == "hidden") && (events.empty() || events[events.size() - 1].asString() != name))
				events.append(name);
		}
		if (games == vr::k_ulOverlayHandleInvalid && ov->FindOverlay("oc.test.game.dashboard", &games) == vr::VROverlayError_None) {
			char key[128] = {};
			vr::EVROverlayError keyError = vr::VROverlayError_None;
			ov->GetOverlayKey(games, key, sizeof(key), &keyError);
			gamesKey = key;
			// Nothing of another program's overlay can be changed
			r["game_overlay_set_refused"] = ov->SetOverlayWidthInMeters(games, 3) != vr::VROverlayError_None;
		}
		if (sawOpen && !open && closedAt == std::chrono::steady_clock::time_point::max())
			closedAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	r["shown"] = sawOpen;
	r["active"] = active;
	r["visible_while_shown"] = visible;
	r["visible_after"] = ov->IsOverlayVisible(main);
	r["at_panel_while_shown"] = atPanelWhileShown;
	r["events"] = events;
	r["game_overlay_found"] = games != vr::k_ulOverlayHandleInvalid && games != main && games != thumbnail;
	r["game_overlay_key"] = gamesKey;
	vr::VROverlayHandle_t missing = vr::k_ulOverlayHandleInvalid;
	r["missing_overlay_found"] = ov->FindOverlay("oc.test.no.such.overlay", &missing) == vr::VROverlayError_None;
	ov->DestroyOverlay(thumbnail);
	ov->DestroyOverlay(main);
	c.core->Cleanup();
	// Its quad at the panel's pose, in this program's own frames
	bool atPanel = false;
	Json::Value dump = ReadStandInDump();
	const Json::Value want = RoundedPosition(panel);
	for (const Json::Value& layer : dump["layers"])
		atPanel = atPanel || (layer["type"].asString() == "quad" && !want.isNull() && RoundedPosition(layer["pose"]) == want);
	r["quad_at_panel"] = atPanel;
	c.Record("dashboard overlay", r);
}

// Whether the Scene document names a running program other than this one
bool SceneAppInStore()
{
	Json::Value scene = StoreDocument("scene");
	return scene["pid"].isUInt() && scene["pid"].asUInt() != OwnPid();
}

// The game's side of settings-live (scripts/helper_apps.json): frames at about 90 a second, each of the size
// GetRecommendedRenderTargetSize answers, asked again on VREvent_SteamVRSectionSettingChanged. The overlay app sets
// supersampleScale 0.5 about 10 frames in: the game's textures, and so its projection views, have half the pixels
// within 5 frames of the setting reaching the store.
void SettingsLiveCase(CaseContext& c)
{
	std::string why;
	std::unique_ptr<GfxApi> gfx = GfxApi::Create(GfxKind::D3D11, &why);
	if (!gfx) {
		Json::Value r;
		r["skipped"] = why;
		c.Record("render scale", r);
		return;
	}
	SceneApp app;
	if (!OpenSceneApp(c, app))
		return;
	auto* settings = Interface<vr::IVRSettings_003::IVRSettings>(c, "IVRSettings_003");
	uint32_t w = 0, h = 0;
	app.sys->GetRecommendedRenderTargetSize(&w, &h);
	const double startPixels = (double)w * h;
	GfxTexture left = gfx->Pattern(w, h, 29, 0), right = gfx->Pattern(w, h, 29, 1);
	gfx->Flush();
	Json::Value r;
	int seenFrame = -1, resizedFrame = -1;
	// Until 10 frames after the setting reached the store, at most 600 (the other program may start late)
	for (int frame = 1; frame <= 600 && !(seenFrame > 0 && frame > seenFrame + 10); frame++) {
		vr::TrackedDevicePose_t poses[16];
		app.comp->WaitGetPoses(poses, 16, nullptr, 0);
		bool changed = false;
		vr::VREvent_t ev{};
		while (app.sys->PollNextEvent(&ev, sizeof(ev)))
			changed = changed || ev.eventType == vr::VREvent_SteamVRSectionSettingChanged;
		if (seenFrame < 0 && settings && std::fabs(settings->GetFloat("steamvr", "supersampleScale") - 0.5f) < 1e-3f)
			seenFrame = frame;
		if (changed) {
			uint32_t nw = 0, nh = 0;
			app.sys->GetRecommendedRenderTargetSize(&nw, &nh);
			if (nw != w || nh != h) {
				w = nw;
				h = nh;
				left = gfx->Pattern(w, h, 29, 0);
				right = gfx->Pattern(w, h, 29, 1);
				gfx->Flush();
				if (resizedFrame < 0)
					resizedFrame = frame;
			}
		}
		app.comp->Submit(vr::Eye_Left, left.Texture(), nullptr, vr::Submit_Default);
		app.comp->Submit(vr::Eye_Right, right.Texture(), nullptr, vr::Submit_Default);
		std::this_thread::sleep_for(std::chrono::milliseconds(11));
	}
	c.core->Cleanup();
	r["setting_seen"] = seenFrame > 0;
	r["resized_within_5_frames"] = seenFrame > 0 && resizedFrame >= seenFrame && resizedFrame - seenFrame <= 5;
	// The last projection layer's first view, against the size recommended at the start
	Json::Value dump = ReadStandInDump();
	Json::Value last;
	for (const Json::Value& layer : dump["layers"])
		if (layer["type"].asString() == "projection")
			last = layer;
	const Json::Value& rect = last["views"][0]["rect"];
	const double lastPixels = rect.size() == 4 ? rect[2].asDouble() * rect[3].asDouble() : 0.0;
	r["pixels_ratio"] = startPixels > 0 ? std::round(lastPixels / startPixels * 100) / 100 : 0.0;
	r["invalid"] = last["invalid"].isNull() ? Json::Value(Json::arrayValue) : last["invalid"];
	c.Record("render scale", r);
	gfx->Flush();
}

// The other program of settings-live: an overlay app that sets steamvr/supersampleScale to 0.5 about 10 frames after
// the game has started
void SettingsLiveSetterCase(CaseContext& c)
{
	Json::Value r;
	r["init"] = (int)c.core->Init(vr::VRApplication_Overlay, nullptr);
	auto* settings = r["init"].asInt() == vr::VRInitError_None ? Interface<vr::IVRSettings_003::IVRSettings>(c, "IVRSettings_003") : nullptr;
	if (!settings) {
		c.Record("set", r);
		if (r["init"].asInt() == vr::VRInitError_None)
			c.core->Cleanup();
		return;
	}
	auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!SceneAppInStore() && std::chrono::steady_clock::now() < until)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	r["saw_game"] = SceneAppInStore();
	std::this_thread::sleep_for(std::chrono::milliseconds(110));
	vr::IVRSettings_003::EVRSettingsError error = vr::IVRSettings_003::VRSettingsError_None;
	settings->SetFloat("steamvr", "supersampleScale", 0.5f, &error);
	r["error"] = (int)error;
	c.core->Cleanup();
	c.Record("set", r);
}

// The play area's walls (scripts/bounds.json: a 2 x 3 m stage, the HMD 0.2 m from its +X edge): 30 frames, then the
// last frame's walls (2.5 m tall quads) as the stand-in got them. With drawBounds one, on the +X edge; without, none.
// With commitRoom (scripts/bounds_room_setup.json: no stage bounds), the play area is a committed 2 x 3 m room setup.
void BoundsCase(CaseContext& c, bool commitRoom)
{
	SceneApp app;
	if (!OpenSceneApp(c, app))
		return;
	if (commitRoom) {
		// A 2 x 3 m room setup, committed as a room setup tool would
		vr::EVRInitError le = vr::VRInitError_None;
		auto* setup = (vr::IVRChaperoneSetup_006::IVRChaperoneSetup*)c.core->GetGenericInterface("IVRChaperoneSetup_006", &le);
		if (setup) {
			setup->SetWorkingPlayAreaSize(2, 3);
			setup->CommitWorkingCopy(vr::IVRChaperoneSetup_006::EChaperoneConfigFile_Live);
		}
	}
	callers::Texture tex(ArgSet::Valid, *c.call);
	for (int frame = 1; frame <= 30; frame++)
		SceneFrame(app, tex.get());
	c.core->Cleanup();
	Json::Value r;
	Json::Value dump = ReadStandInDump();
	const Json::Value& layers = dump["layers"];
	const Json::Value lastFrame = layers.size() ? layers[layers.size() - 1]["frame"] : Json::Value();
	int walls = 0;
	Json::Value positions(Json::arrayValue);
	for (const Json::Value& layer : layers) {
		if (layer["frame"] != lastFrame || layer["type"].asString() != "quad" || layer["size"].size() != 2)
			continue;
		if (std::fabs(layer["size"][1].asDouble() - 2.5) > 1e-3)
			continue;
		walls++;
		positions.append(RoundedPosition(layer["pose"]));
	}
	r["walls"] = walls;
	r["positions"] = positions;
	c.Record("walls", r);
}

// An overlay application with one overlay from memory and nothing else: no WaitGetPoses, so every frame is
// OpenLamina's own. About 60 frames' time at 90 Hz.
void OverlayAppCase(CaseContext& c)
{
	using namespace vr::IVROverlay_028;
	Json::Value r;
	r["init"] = (int)c.core->Init(vr::VRApplication_Overlay, nullptr);
	if (r["init"].asInt() != vr::VRInitError_None) {
		c.Record("frames", r);
		return;
	}
	auto* ov = Interface<IVROverlay>(c, "IVROverlay_028");
	r["overlay_interface"] = ov != nullptr;
	if (ov) {
		vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid;
		r["create"] = (int)ov->CreateOverlay("oc.helper.raw", "Helper", &h);
		std::vector<uint8_t> grey(64 * 64 * 4, 0x80);
		r["raw"] = (int)ov->SetOverlayRaw(h, grey.data(), 64, 64, 4);
		vr::HmdMatrix34_t m = At(0, 1.5f, -2);
		ov->SetOverlayTransformAbsolute(h, vr::TrackingUniverseStanding, &m);
		r["show"] = (int)ov->ShowOverlay(h);
		std::this_thread::sleep_for(std::chrono::milliseconds(667));
		ov->DestroyOverlay(h);
	}
	c.core->Cleanup();
	Json::Value dump = DumpSummary();
	r["session_overlay"] = dump["overlay"];
	r["layer_types"] = dump["layer_types"];
	r["frame_order_errors"] = dump["frame_order_errors"];
	r["frames_at_least_40"] = dump["frames_ended"].asInt() >= 40;
	c.Record("frames", r);
}

// An overlay application's textures of each API, at once: each is copied onto OpenLamina's own D3D11 device (the
// overlay session's), and its quad shows the test pattern. A kind whose device can't be made here skips the case.
struct OverlayTextureKind {
	GfxKind kind;
	int64_t format;
	const char* name;
};

void OverlayAppTexturesCase(CaseContext& c, const char* label, std::vector<OverlayTextureKind> kinds)
{
	using namespace vr::IVROverlay_028;
	std::vector<std::unique_ptr<GfxApi>> apis;
	std::vector<GfxTexture> textures;
	for (const OverlayTextureKind& k : kinds) {
		std::string why;
		apis.push_back(GfxApi::Create(k.kind, &why));
		if (!apis.back()) {
			Json::Value r;
			r["skipped"] = why;
			c.Record(label, r);
			return;
		}
		textures.push_back(apis.back()->Pattern(64, 64, k.format, 0));
		apis.back()->Flush();
	}
	Json::Value r;
	r["init"] = (int)c.core->Init(vr::VRApplication_Overlay, nullptr);
	if (r["init"].asInt() != vr::VRInitError_None) {
		c.Record(label, r);
		return;
	}
	auto* ov = Interface<IVROverlay>(c, "IVROverlay_028");
	if (ov) {
		std::vector<vr::VROverlayHandle_t> handles(kinds.size(), vr::k_ulOverlayHandleInvalid);
		for (size_t i = 0; i < kinds.size(); i++) {
			ov->CreateOverlay((std::string("oc.helper.") + kinds[i].name).c_str(), kinds[i].name, &handles[i]);
			vr::HmdMatrix34_t m = At((float)i * 1.2f, 1.5f, -2);
			ov->SetOverlayTransformAbsolute(handles[i], vr::TrackingUniverseStanding, &m);
			r[kinds[i].name] = (int)ov->SetOverlayTexture(handles[i], textures[i].Texture());
			ov->ShowOverlay(handles[i]);
		}
		// Again, as programs set their textures every frame
		r["again"] = (int)ov->SetOverlayTexture(handles[0], textures[0].Texture());
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		for (vr::VROverlayHandle_t h : handles)
			ov->DestroyOverlay(h);
	}
	c.core->Cleanup();
	Json::Value summary = DumpSummary();
	r["binding"] = summary["binding"];
	r["session_overlay"] = summary["overlay"];
	r["frame_order_errors"] = summary["frame_order_errors"];
	// The quads that show the pattern (red top left, white bottom right) in the last frame that has them all
	Json::Value dump = ReadStandInDump();
	const Json::Value& layers = dump["layers"];
	const uint64_t lastFrame = LastFrameWithEveryQuad(dump);
	int shown = 0;
	for (const Json::Value& layer : layers) {
		if (layer["frame"].asUInt64() == lastFrame && layer["type"].asString() == "quad" && layer["samples"]["tl"].asString() == "ff0000ff"
		    && layer["samples"]["br"].asString() == "ffffffff")
			shown++;
	}
	r["quads_with_pattern"] = shown;
	c.Record(label, r);
	for (auto& api : apis)
		api->Flush();
}

// An overlay application that reads its overlay's texture back and writes it in place, and shows it on a second
// overlay by its DXGI shared handle, as desktop viewers do: the pattern of eye 0 is set on "source", read back with
// GetOverlayTexture, and the pattern of eye 1 copied straight into the texture that came back; "target" gets that
// texture's shared handle, with bounds of its left half. Both quads then show eye 1's pattern (top-left blue), the
// target's its left half (blue over white), with no further SetOverlayTexture. D3D11 only.
void OverlayAppSharedTextureCase(CaseContext& c)
{
	Json::Value r;
#ifdef _WIN32
	using namespace vr::IVROverlay_028;
	std::string why;
	std::unique_ptr<GfxApi> api = GfxApi::Create(GfxKind::D3D11, &why);
	if (!api) {
		r["skipped"] = why;
		c.Record("shared", r);
		return;
	}
	const int64_t bgra8 = 87; // DXGI_FORMAT_B8G8R8A8_UNORM
	GfxTexture first = api->Pattern(64, 64, bgra8, 0);
	GfxTexture second = api->Pattern(64, 64, bgra8, 1);
	api->Flush();
	r["init"] = (int)c.core->Init(vr::VRApplication_Overlay, nullptr);
	if (r["init"].asInt() != vr::VRInitError_None) {
		c.Record("shared", r);
		return;
	}
	auto* ov = Interface<IVROverlay>(c, "IVROverlay_028");
	if (ov) {
		vr::VROverlayHandle_t source = vr::k_ulOverlayHandleInvalid, target = vr::k_ulOverlayHandleInvalid;
		ov->CreateOverlay("oc.helper.source", "Source", &source);
		ov->CreateOverlay("oc.helper.target", "Target", &target);
		vr::HmdMatrix34_t at = At(0, 1.5f, -2);
		ov->SetOverlayTransformAbsolute(source, vr::TrackingUniverseStanding, &at);
		at = At(1.2f, 1.5f, -2);
		ov->SetOverlayTransformAbsolute(target, vr::TrackingUniverseStanding, &at);
		r["set_source"] = (int)ov->SetOverlayTexture(source, first.Texture());
		ov->ShowOverlay(source);

		// Read back: a view of a texture on the program's own device, which has a shared handle
		auto* own = (ID3D11Texture2D*)first.keep;
		ID3D11ShaderResourceView* view = nullptr;
		uint32_t width = 0, height = 0, format = 0;
		vr::ETextureType type = vr::TextureType_Invalid;
		vr::EColorSpace space = vr::ColorSpace_Auto;
		vr::VRTextureBounds_t bounds{};
		r["get"] = (int)ov->GetOverlayTexture(source, (void**)&view, own, &width, &height, &format, &type, &space, &bounds);
		r["size"] = Json::Value(Json::arrayValue);
		r["size"].append(width);
		r["size"].append(height);
		ID3D11Resource* live = nullptr;
		HANDLE handle = nullptr;
		if (view) {
			view->GetResource(&live);
			IDXGIResource* dxgi = nullptr;
			if (live && SUCCEEDED(live->QueryInterface(__uuidof(IDXGIResource), (void**)&dxgi)) && dxgi) {
				dxgi->GetSharedHandle(&handle);
				dxgi->Release();
			}
		}
		r["view"] = view != nullptr;
		r["not_the_programs_own_texture"] = live != nullptr && live != (ID3D11Resource*)own;
		r["shared_handle"] = handle != nullptr;

		// The same texture again while the first view is out
		ID3D11ShaderResourceView* again = nullptr;
		ov->GetOverlayTexture(source, (void**)&again, own, &width, &height, &format, &type, &space, &bounds);
		ID3D11Resource* liveAgain = nullptr;
		if (again)
			again->GetResource(&liveAgain);
		r["same_texture_again"] = liveAgain != nullptr && liveAgain == live;
		if (liveAgain)
			liveAgain->Release();
		if (again)
			r["release_again"] = (int)ov->ReleaseNativeOverlayHandle(source, again);

		// On the second overlay by its handle, its left half
		vr::Texture_t shared{ handle, vr::TextureType_DXGISharedHandle, vr::ColorSpace_Gamma };
		const vr::VRTextureBounds_t leftHalf{ 0, 0, 0.5f, 1 };
		ov->SetOverlayTextureBounds(target, &leftHalf);
		r["set_target"] = (int)ov->SetOverlayTexture(target, &shared);
		ov->ShowOverlay(target);
		// A null handle is refused
		vr::Texture_t none{ nullptr, vr::TextureType_DXGISharedHandle, vr::ColorSpace_Gamma };
		r["set_null_handle_refused"] = ov->SetOverlayTexture(target, &none) != vr::VROverlayError_None;

		// Written in place, with no call that says so
		if (live) {
			ID3D11Device* device = nullptr;
			ID3D11DeviceContext* context = nullptr;
			own->GetDevice(&device);
			device->GetImmediateContext(&context);
			context->CopyResource(live, (ID3D11Texture2D*)second.keep);
			context->Flush();
			context->Release();
			device->Release();
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(400));
		if (live)
			live->Release();
		if (view)
			r["release"] = (int)ov->ReleaseNativeOverlayHandle(source, view);
		ov->DestroyOverlay(target);
		ov->DestroyOverlay(source);
	}
	c.core->Cleanup();
	Json::Value dump = ReadStandInDump();
	r["frame_order_errors"] = dump["frame_order_errors"];
	r["frames_refused"] = (Json::UInt64)dump["frames_refused"].asUInt64();
	// The quads of the last frame that has both, told apart by where they are
	const Json::Value& layers = dump["layers"];
	const uint64_t lastFrame = LastFrameWithEveryQuad(dump);
	for (const Json::Value& layer : layers) {
		if (layer["frame"].asUInt64() != lastFrame || layer["type"].asString() != "quad")
			continue;
		const Json::Value& s = layer["samples"];
		const Json::Value position = RoundedPosition(layer["pose"]);
		if (position.isNull())
			continue;
		if (position[0].asDouble() < 0.5)
			r["source_shows_what_was_written"] = s["tl"].asString() == "0000ffff" && s["tr"].asString() == "ff0000ff"
			    && s["bl"].asString() == "ffffffff" && s["br"].asString() == "00ff00ff";
		else
			r["target_shows_its_left_half"] = s["tl"].asString() == "0000ffff" && s["tr"].asString() == "0000ffff"
			    && s["bl"].asString() == "ffffffff" && s["br"].asString() == "ffffffff";
	}
	api->Flush();
#else
	r["skipped"] = "Direct3D 11 only";
#endif
	c.Record("shared", r);
}

// A file in the store's folder that one program of a pair writes for the other
std::string StoreMarker(const char* name)
{
	const char* dir = std::getenv("OC_STORE_DIR");
	return dir && *dir ? std::string(dir) + "/" + name : std::string();
}

// A background application, once a game (background-app-game) runs: a headless session, whose poses and events work,
// and which can't submit. It asks again on 121, as such programs do, for at most 10 s.
void BackgroundAppCase(CaseContext& c)
{
	Json::Value r;
	vr::EVRInitError init = vr::VRInitError_Init_NoServerForBackgroundApp;
	const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (std::chrono::steady_clock::now() < until) {
		init = c.core->Init(vr::VRApplication_Background, nullptr);
		if (init != vr::VRInitError_Init_NoServerForBackgroundApp)
			break;
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	r["init"] = (int)init;
	// The game can go now
	if (std::string marker = StoreMarker("background-joined"); !marker.empty())
		std::ofstream(marker) << "1";
	if (init != vr::VRInitError_None) {
		c.Record("session", r);
		return;
	}
	auto* sys = Interface<vr::IVRSystem_022::IVRSystem>(c, "IVRSystem_022");
	auto* comp = Interface<vr::IVRCompositor_029::IVRCompositor>(c, "IVRCompositor_029");
	r["system"] = sys != nullptr;
	r["compositor"] = comp != nullptr;
	bool valid = false;
	for (int i = 0; i < 50 && sys && !valid; i++) {
		vr::VREvent_t ev{};
		while (sys->PollNextEvent(&ev, sizeof(ev))) {
		}
		vr::TrackedDevicePose_t pose{};
		sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, &pose, 1);
		valid = pose.bPoseIsValid;
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	r["hmd_pose_valid"] = valid;
	if (sys)
		r["hmd_connected"] = sys->IsTrackedDeviceConnected(0);
	if (comp) {
		vr::TrackedDevicePose_t poses[4];
		r["wait_get_poses"] = (int)comp->WaitGetPoses(poses, 4, nullptr, 0);
		callers::Texture tex(ArgSet::Valid, *c.call);
		r["submit"] = (int)comp->Submit(vr::Eye_Left, tex.get(), nullptr, vr::Submit_Default);
	}
	c.core->Cleanup();
	Json::Value dump = DumpSummary();
	r["binding"] = dump["binding"];
	r["session_overlay"] = dump["overlay"];
	// No frame calls on a headless session (Monado answers them CALL_ORDER_INVALID), and its poses located at the time
	// now, not at its session's start
	Json::Value raw = ReadStandInDump();
	r["frame_calls"] = raw["frames_waited"].asInt() + raw["frames_begun"].asInt() + raw["frames_ended"].asInt();
	r["frame_order_errors"] = raw["frame_order_errors"].asInt();
	r["stale_locates"] = raw["stale_locates"].asInt();
	c.Record("session", r);
}

// The game background-app waits for: it runs frames until the background application has joined (at most 15 s)
void BackgroundAppGameCase(CaseContext& c)
{
	SceneApp app;
	if (!OpenSceneApp(c, app))
		return;
	callers::Texture tex(ArgSet::Valid, *c.call);
	const std::string marker = StoreMarker("background-joined");
	bool joined = false;
	const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	while (!joined && std::chrono::steady_clock::now() < until) {
		SceneFrame(app, tex.get());
		joined = !marker.empty() && std::ifstream(marker).good();
	}
	c.core->Cleanup();
	Json::Value r;
	r["background_joined"] = joined;
	c.Record("frames", r);
}

// A background application with no other OpenLamina program running: 121, without making an OpenXR instance (which
// would start the runtime)
void BackgroundAppAloneCase(CaseContext& c)
{
	Json::Value r;
	r["init"] = (int)c.core->Init(vr::VRApplication_Background, nullptr);
	if (r["init"].asInt() == vr::VRInitError_None)
		c.core->Cleanup();
	r["instance_made"] = ReadStandInDump().isMember("instance");
	c.Record("init", r);
}

// Without the runtime's overlay and headless extensions (or with a runtime that fails to start): the refusals,
// 130 InvalidApplicationType and 121 NoServerForBackgroundApp
void HelperAppsUnsupportedCase(CaseContext& c)
{
	Json::Value r;
	for (vr::EVRApplicationType t : { vr::VRApplication_Overlay, vr::VRApplication_Background }) {
		vr::EVRInitError e = c.core->Init(t, nullptr);
		r[t == vr::VRApplication_Overlay ? "overlay" : "background"] = (int)e;
		if (e == vr::VRInitError_None)
			c.core->Cleanup();
	}
	c.Record("init", r);
}

} // namespace

const std::map<std::string, CaseFn>& conformance::HelperCases()
{
	static const std::map<std::string, CaseFn> cases = {
		{ "overlay-app", OverlayAppCase },
		{ "overlay-app-two-apis", [](CaseContext& c) {
			 OverlayAppTexturesCase(c, "textures", { { GfxKind::D3D11, 29, "d3d11" }, { GfxKind::D3D12, 29, "d3d12" } });
		 } },
		{ "overlay-app-vk", [](CaseContext& c) { OverlayAppTexturesCase(c, "textures", { { GfxKind::Vulkan, 43, "vulkan" } }); } },
		{ "overlay-app-gl", [](CaseContext& c) { OverlayAppTexturesCase(c, "textures", { { GfxKind::OpenGL, 0x8C43, "gl" } }); } },
		{ "overlay-app-shared", OverlayAppSharedTextureCase },
		{ "background-app", BackgroundAppCase },
		{ "background-app-game", BackgroundAppGameCase },
		{ "background-app-alone", BackgroundAppAloneCase },
		{ "helper-apps-unsupported", HelperAppsUnsupportedCase },
		{ "remote-pointer", RemotePointerSceneCase },
		{ "remote-pointer-overlay", RemotePointerOverlayCase },
		{ "remote-pointer-peer-dies", RemotePointerPeerDiesCase },
		{ "remote-pointer-dying-overlay", RemotePointerDyingOverlayCase },
		{ "dashboard", DashboardSceneCase },
		{ "dashboard-overlay", DashboardOverlayCase },
		{ "settings-live", SettingsLiveCase },
		{ "settings-live-setter", SettingsLiveSetterCase },
		{ "bounds", [](CaseContext& c) { BoundsCase(c, false); } },
		{ "bounds-room-setup", [](CaseContext& c) { BoundsCase(c, true); } },
	};
	return cases;
}
