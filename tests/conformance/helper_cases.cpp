// The helper programs' cases: overlay and background applications, alone and beside a scene app (run.py --jobs 2 with
// a shared store). Each writes one record per step, as cases.cpp's do.

#include "conformance.h"

#include "IVRCompositor_029.h"
#include "IVRInput_011.h"
#include "IVROverlay_028.h"
#include "IVRSystem_022.h"

#include "gfx/GfxApi.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
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
// 150. 260 frames, at about 90 a second, once the overlay app's dashboard overlay is in the store.
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
		if (frame == 120) {
			Json::Value doc = StoreDocument("dashboard");
			r["open_at_120"] = doc["open"];
			r["panel_position"] = RoundedPosition(doc["panel_pose"]);
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
	bool sawOpen = false, active = false;
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
		}
		if (sawOpen && !open && closedAt == std::chrono::steady_clock::time_point::max())
			closedAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	r["shown"] = sawOpen;
	r["active"] = active;
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
	// The last frame's quads that show the pattern: red top left, white bottom right
	Json::Value dump = ReadStandInDump();
	const Json::Value& layers = dump["layers"];
	const Json::Value lastFrame = layers.size() ? layers[layers.size() - 1]["frame"] : Json::Value();
	int shown = 0;
	for (const Json::Value& layer : layers) {
		if (layer["frame"] == lastFrame && layer["type"].asString() == "quad" && layer["samples"]["tl"].asString() == "ff0000ff"
		    && layer["samples"]["br"].asString() == "ffffffff")
			shown++;
	}
	r["quads_with_pattern"] = shown;
	c.Record(label, r);
	for (auto& api : apis)
		api->Flush();
}

// A background application: a headless session, whose poses and events work, and which can't submit
void BackgroundAppCase(CaseContext& c)
{
	Json::Value r;
	r["init"] = (int)c.core->Init(vr::VRApplication_Background, nullptr);
	if (r["init"].asInt() != vr::VRInitError_None) {
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
		{ "background-app", BackgroundAppCase },
		{ "helper-apps-unsupported", HelperAppsUnsupportedCase },
		{ "remote-pointer", RemotePointerSceneCase },
		{ "remote-pointer-overlay", RemotePointerOverlayCase },
		{ "remote-pointer-peer-dies", RemotePointerPeerDiesCase },
		{ "remote-pointer-dying-overlay", RemotePointerDyingOverlayCase },
		{ "dashboard", DashboardSceneCase },
		{ "dashboard-overlay", DashboardOverlayCase },
	};
	return cases;
}
