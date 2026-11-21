#include "stdafx.h"
#include "Misc/EnumNames.h"
#define BASE_IMPL
#include "BaseApplications.h"
#include "BaseClientCore.h"
#include "Misc/ProgramEvents.h"
#include "Misc/AppManifest.h"
#include "Misc/Config.h"
#include "Misc/Dashboard.h"
#include "Misc/Process.h"
#include "Misc/SharedStore.h"

#include <atomic>
#include <cctype>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <set>
#include <string>

using namespace vr;

// So I can easily use it for return types
typedef OOVR_EVRApplicationError EVRApplicationError;

// The application registry lives in the store's Applications document, shared by every OpenComposite program
// of the user:
//   {"manifests": {"<path>": {"temporary": bool, "owner_pid": n, "apps": [entries]}},
//    "autolaunch": {"<key>": bool}, "mime_defaults": {"<type>": "<key>"}, "pids": {"<pid>": "<key>"}, "pid_starts": {"<pid>": start_time},
//    "last_launch": {"<key>": unix_seconds}, "pid_auto": {"<pid>": true}, "launch_args": {"<handle>": {"pid": n, "args": "..."}},
//    "next_args_handle": n}
// (pid_auto marks a process listed by itself at init, not by IdentifyApplication: its key is worked out again), and
// the scene application in the Scene document: {"pid": n, "app_key": "...", "since_ms": n, "transition": {...}}, the
// transition being the change to another scene application under way: {"state": n, "app_key": "<the new one's>",
// "old_pid": n, "old_start": n, "launch": bool, "since_ms": n}.

namespace {

// Copies text into an app's buffer: cut short and terminated if it doesn't fit. True if it fitted.
bool CopyOut(const std::string& text, char* buffer, uint32_t size)
{
	if (buffer && size) {
		size_t n = std::min<size_t>(text.size(), size - 1);
		memcpy(buffer, text.data(), n);
		buffer[n] = 0;
	}
	return buffer && size >= text.size() + 1;
}

void Clear(char* buffer, uint32_t size)
{
	if (buffer && size)
		buffer[0] = 0;
}

std::string ManifestKey(const char* path)
{
	// The full path as given, normalised, so adding the same file twice is one manifest
	std::filesystem::path p = PathFromUtf8(path);
	std::error_code ec;
	if (p.is_relative())
		p = std::filesystem::absolute(p, ec);
	return PathToUtf8(p.lexically_normal());
}

uint64_t UnixSeconds()
{
	using namespace std::chrono;
	return (uint64_t)duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

uint64_t NowMs()
{
	using namespace std::chrono;
	return (uint64_t)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// Events of older OpenVR versions, which programs built with them still wait for
constexpr vr::EVREventType kTransitionStarted = (vr::EVREventType)1300;
constexpr vr::EVREventType kTransitionAborted = (vr::EVREventType)1301;
constexpr vr::EVREventType kTransitionNewAppStarted = (vr::EVREventType)1302;
constexpr vr::EVREventType kTransitionNewAppLaunchComplete = (vr::EVREventType)1305;

// How long a launched application has to become the scene application before the transition is over anyway
constexpr uint64_t kNewAppLaunchMs = 30000;
// How long after the old application was first asked to quit the transition lasts: it holds other launches off
// while the old application runs, and its launch is taken over if the old one ended without launching it
constexpr uint64_t kTakeOverMs = 5 * 60 * 1000;
// How long an old application asked to quit for a launch has to do so before it's ended (ForceQuitProcess's way)
constexpr uint64_t kForceQuitMs = 10000;

// Launch arguments kept for running applications at most: the oldest go first
constexpr size_t kMaxLaunchArgs = 32;

// A process id kept as a document key; 0 for anything else
uint32_t ParsePid(const std::string& text)
{
	char* end = nullptr;
	unsigned long v = strtoul(text.c_str(), &end, 10);
	return end && *end == '\0' && v <= UINT32_MAX ? (uint32_t)v : 0;
}

// A process id kept as a document value; 0 for anything else
uint32_t PidOf(const Json::Value& v)
{
	return v.isUInt() ? v.asUInt() : 0;
}

// A document value of the type asked for, else the fallback: jsoncpp throws converting most others
std::string StringOr(const Json::Value& v, const std::string& fallback = std::string())
{
	return v.isString() ? v.asString() : fallback;
}

bool BoolOr(const Json::Value& v, bool fallback)
{
	return v.isBool() ? v.asBool() : fallback;
}

uint64_t UInt64Or(const Json::Value& v, uint64_t fallback)
{
	return v.isUInt64() ? v.asUInt64() : fallback;
}

// A process id listed in the Applications document's pids, with the start time it had then: a process that took
// over the id since is another one
void RecordProcess(Json::Value& doc, uint32_t pid, const std::string& key)
{
	std::string name = std::to_string(pid);
	doc["pids"][name] = key;
	if (doc["pid_auto"].isObject())
		doc["pid_auto"].removeMember(name);
	uint64_t start = SharedStore::ProcessStartTime(pid);
	if (start)
		doc["pid_starts"][name] = (Json::UInt64)start;
	else if (doc["pid_starts"].isObject())
		doc["pid_starts"].removeMember(name);
}

// Whether the process listed under that name still runs, and is the one that was listed
bool ListedProcessAlive(const Json::Value& doc, const std::string& name)
{
	const Json::Value& starts = doc["pid_starts"];
	return SharedStore::IsSameProcessAlive(ParsePid(name), starts.isObject() ? UInt64Or(starts[name], 0) : 0);
}

// Tells this program and every other one that the list of applications changed
void PostApplicationListUpdated()
{
	OcPostEvent(vr::VREvent_ApplicationListUpdated, vr::k_unTrackedDeviceIndexInvalid, vr::VREvent_Data_t{}, true);
}

// Edits the Applications document, first dropping what programs that ended left: their process ids, and the
// temporary manifests they added
bool UpdateApplications(SharedStore& store, const std::function<void(Json::Value& doc)>& edit)
{
	return store.Update(StoreDoc::Applications, [&](Json::Value& doc) {
		if (doc.isMember("pids") && doc["pids"].isObject()) {
			for (const std::string& p : doc["pids"].getMemberNames())
				if (!ListedProcessAlive(doc, p))
					doc["pids"].removeMember(p);
		}
		// The start times of the processes that are no longer listed
		for (const char* listing : { "pid_starts", "pid_auto" }) {
			if (doc.isMember(listing) && doc[listing].isObject() && doc["pids"].isObject()) {
				for (const std::string& p : doc[listing].getMemberNames())
					if (!doc["pids"].isMember(p))
						doc[listing].removeMember(p);
			}
		}
		// Launch arguments for programs that ended
		if (doc.isMember("launch_args") && doc["launch_args"].isObject()) {
			for (const std::string& h : doc["launch_args"].getMemberNames()) {
				const Json::Value& a = doc["launch_args"][h];
				if (!a.isObject() || !SharedStore::IsProcessAlive(PidOf(a["pid"])))
					doc["launch_args"].removeMember(h);
			}
		}
		if (doc.isMember("manifests") && doc["manifests"].isObject()) {
			for (const std::string& path : doc["manifests"].getMemberNames()) {
				const Json::Value& m = doc["manifests"][path];
				if (m.isObject() && m["temporary"].isBool() && m["temporary"].asBool() && !SharedStore::IsProcessAlive(PidOf(m["owner_pid"])))
					doc["manifests"].removeMember(path);
			}
		}
		edit(doc);
	});
}

std::string Join(const std::vector<std::string>& items)
{
	std::string out;
	for (const std::string& s : items) {
		if (!out.empty())
			out += ",";
		out += s;
	}
	return out;
}

// The MIME types an app opens: its manifest's, and those it was made the default for
std::vector<std::string> MimeTypesOf(const AppManifestEntry& app, const Json::Value& doc)
{
	std::vector<std::string> types = app.mimeTypes;
	const Json::Value& defaults = doc["mime_defaults"];
	if (defaults.isObject()) {
		for (const std::string& type : defaults.getMemberNames()) {
			if (StringOr(defaults[type]) == app.appKey && std::find(types.begin(), types.end(), type) == types.end())
				types.push_back(type);
		}
	}
	return types;
}

// The scene application named in the Scene document, if it still runs; 0 if none
uint32_t LiveScenePid(const Json::Value& scene)
{
	uint32_t pid = PidOf(scene["pid"]);
	if (pid == oc_current_pid())
		return pid;
	return pid && SharedStore::IsSameProcessAlive(pid, UInt64Or(scene["start"], 0)) ? pid : 0;
}

// The change of scene application under way, from the Scene document: none once the old application ended without
// launching the new one, or the new one had its time to start
struct Transition {
	BaseApplications::EVRApplicationTransitionState state = VRApplicationTransition_None;
	std::string appKey;
	uint32_t oldPid = 0;
	bool launch = false;
};

// A transition ended without its new application: the scene application state is what it was before
void AnnounceTransitionAborted()
{
	OcPostEvent(kTransitionAborted, vr::k_unTrackedDeviceIndexInvalid, vr::VREvent_Data_t{}, true);
	OcPostEvent(vr::VREvent_SceneApplicationStateChanged, vr::k_unTrackedDeviceIndexInvalid, vr::VREvent_Data_t{}, true);
}

Transition LiveTransition(const Json::Value& scene)
{
	Transition t;
	const Json::Value& tr = scene["transition"];
	if (!tr.isObject() || !tr["state"].isInt())
		return t;
	const int state = tr["state"].asInt();
	const uint32_t oldPid = PidOf(tr["old_pid"]);
	bool live = false;
	if (state == VRApplicationTransition_OldAppQuitSent)
		live = NowMs() < UInt64Or(tr["since_ms"], 0) + kTakeOverMs
		    && (oldPid == oc_current_pid() || SharedStore::IsSameProcessAlive(oldPid, UInt64Or(tr["old_start"], 0)));
	else if (state == VRApplicationTransition_NewAppLaunched)
		live = NowMs() < UInt64Or(tr["since_ms"], 0) + kNewAppLaunchMs;
	if (!live)
		return t;
	t.state = (BaseApplications::EVRApplicationTransitionState)state;
	t.appKey = StringOr(tr["app_key"]);
	t.oldPid = oldPid;
	t.launch = BoolOr(tr["launch"], false);
	return t;
}

// Whether the application is one that becomes the scene application when it runs, so starting it ends the one there is
bool IsSceneApplication(const AppManifestEntry& app)
{
	return !app.isDashboardOverlay && !app.isTemplate && app.launchType != "url";
}

} // namespace

SharedStore& BaseApplications::Store()
{
	return store ? *store : SharedStore::Get();
}

std::optional<AppManifestEntry> BaseApplications::Find(const char* pchAppKey)
{
	if (!pchAppKey || !pchAppKey[0])
		return std::nullopt;
	for (AppManifestEntry& e : InstalledApplications(Store()))
		if (e.appKey == pchAppKey)
			return std::move(e);
	return std::nullopt;
}

namespace {
// On Linux, an application whose binary is a Windows program can't be started: there's no Proton or Wine to start it
// with. Refused (logged) before anything else is done for the launch.
bool WindowsBinaryRefused(const AppManifestEntry& app)
{
#ifdef _WIN32
	(void)app;
	return false;
#else
	if (app.launchType == "url")
		return false;
	std::string ext = app.ResolvedBinary().extension().string();
	std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	if (ext != ".exe")
		return false;
	OOVR_LOGF("Not launching application %s: its binary (%s) is a Windows program, which can't be started on Linux",
	    app.appKey.c_str(), app.ResolvedBinary().string().c_str());
	return true;
#endif
}

// Whether this process has drawn since it became the scene application, and whether the store has that
std::atomic<bool> sceneDrawn{ false }, sceneDrawnStored{ false };
} // namespace

void BaseApplications::SetSceneProcess(bool running)
{
	SetSceneProcess(SharedStore::Get(), running);
}

void BaseApplications::SetSceneProcess(SharedStore& store, bool running)
{
	uint32_t pid = oc_current_pid();
	std::string key = running ? CurrentProcessAppKey(store) : std::string();
	const bool hasDashboard = DashboardButtonFromSetting(oovr_global_configuration.DashboardButton()) != Dashboard::Button::Off;
	bool changed = false;
	uint32_t oldPid = 0;
	// The transition this program was waited on for: it became the new scene application, or was asked to quit
	bool arrived = false, quitAsked = false;
	std::string toLaunch;
	store.Update(StoreDoc::Scene, [&](Json::Value& scene) {
		if (!scene.isObject())
			scene = Json::Value(Json::objectValue);
		Transition t = LiveTransition(scene);
		Json::Value transition = t.state != VRApplicationTransition_None ? scene["transition"] : Json::Value();
		if (running) {
			oldPid = PidOf(scene["pid"]);
			changed = oldPid != pid;
			scene = Json::Value(Json::objectValue);
			scene["pid"] = pid;
			scene["start"] = (Json::UInt64)SharedStore::OwnStartTime();
			// Whether its lasers reach other programs' overlays (laserMouse): they then don't point themselves
			scene["points"] = oovr_global_configuration.LaserMouse();
			// Whether it has a dashboard to show other programs' dashboard overlays in
			scene["dashboard"] = hasDashboard;
			scene["app_key"] = key;
			scene["since_ms"] = (Json::UInt64)NowMs();
			// A new session of the same program: what it has drawn still counts
			if (!changed && sceneDrawn)
				scene["drawn"] = true;
			// A launch made with no scene application running (no old_pid) ends when another program becomes the
			// scene application; a switch's launch (with its old_pid) is kept
			const bool launchWithNoOld = t.state == VRApplicationTransition_NewAppLaunched && t.oldPid == 0;
			if (t.state != VRApplicationTransition_None && t.appKey == key)
				arrived = true;
			else if (!transition.isNull() && !launchWithNoOld)
				scene["transition"] = transition;
		} else {
			// Only our own: another program may have become the scene application since
			if (PidOf(scene["pid"]) == pid) {
				scene = Json::Value(Json::objectValue);
				oldPid = pid;
				changed = true;
			}
			if (t.state == VRApplicationTransition_OldAppQuitSent && t.oldPid == pid) {
				quitAsked = true;
				// The new application starts once this one has gone
				if (t.launch) {
					toLaunch = t.appKey;
					transition["state"] = VRApplicationTransition_NewAppLaunched;
					transition["since_ms"] = (Json::UInt64)NowMs();
					scene["transition"] = transition;
				}
			} else if (!transition.isNull()) {
				scene["transition"] = transition;
			}
		}
	});

	if (changed)
		sceneDrawn = sceneDrawnStored = false;

	// For this program and every other one
	if (changed) {
		vr::VREvent_Data_t data{};
		data.process.pid = running ? pid : 0;
		data.process.oldPid = oldPid;
		OcPostEvent(vr::VREvent_SceneApplicationChanged, vr::k_unTrackedDeviceIndexInvalid, data, true);

		data = {};
		data.process.pid = pid;
		OcPostEvent(running ? vr::VREvent_ProcessConnected : vr::VREvent_ProcessDisconnected, vr::k_unTrackedDeviceIndexInvalid, data, true);

		data = {};
		data.process.pid = pid;
		OcPostEvent(vr::VREvent_SceneApplicationStateChanged, vr::k_unTrackedDeviceIndexInvalid, data, true);
	}

	if (!toLaunch.empty())
		LaunchForTransition(store, toLaunch, pid);
	if (quitAsked) {
		// What a program that asked this one to quit waits for before it tries again
		vr::VREvent_Data_t data{};
		data.process.pid = pid;
		OcPostEvent(vr::VREvent_ProcessQuit, vr::k_unTrackedDeviceIndexInvalid, data, true);
	}
	// A new scene application's session starts the dashboard overlays flagged to start with the dashboard, where it
	// has one
	if (running && changed && hasDashboard) {
		BaseApplications apps(store);
		StartAutolaunchOverlays(apps);
	}
	if (arrived) {
		vr::VREvent_Data_t data{};
		data.process.pid = pid;
		data.process.oldPid = oldPid;
		OcPostEvent(kTransitionNewAppLaunchComplete, vr::k_unTrackedDeviceIndexInvalid, data, true);
	}
}

EVRApplicationError BaseApplications::LaunchForTransition(SharedStore& store, const std::string& appKey, uint32_t oldPid)
{
	BaseApplications apps(store);
	std::optional<AppManifestEntry> app = apps.Find(appKey.c_str());
	uint32_t started = 0;
	// Started already (by hand, say): not a second copy, and its session's start ends the transition
	if (app && apps.GetApplicationProcessId(appKey.c_str()))
		return VRApplicationError_None;
	const EVRApplicationError launched = app ? apps.Launch(*app, "", &started) : VRApplicationError_UnknownApplication;
	if (launched == VRApplicationError_None) {
		vr::VREvent_Data_t data{};
		data.process.pid = started;
		data.process.oldPid = oldPid;
		OcPostEvent(kTransitionNewAppStarted, vr::k_unTrackedDeviceIndexInvalid, data, true);
		return launched;
	}
	store.Update(StoreDoc::Scene, [&](Json::Value& scene) {
		if (scene.isObject() && StringOr(scene["transition"]["app_key"]) == appKey)
			scene.removeMember("transition");
	});
	AnnounceTransitionAborted();
	return launched;
}

void BaseApplications::TakeOverPendingLaunch(SharedStore& store)
{
	// An old scene application that ended without its session's end (it crashed, or was killed) launches nothing: the
	// first program to notice does
	const Json::Value pending = store.ReadAt(StoreDoc::Scene, { "transition" });
	if (!pending.isObject() || !BoolOr(pending["launch"], false) || !pending["state"].isInt()
	    || pending["state"].asInt() != VRApplicationTransition_OldAppQuitSent || PidOf(pending["old_pid"]) == oc_current_pid())
		return;
	if (SharedStore::IsSameProcessAlive(PidOf(pending["old_pid"]), UInt64Or(pending["old_start"], 0))) {
		// An old application that ignored the quit for kForceQuitMs is ended, if it's still the one asked (its start
		// time known); its launch is taken over once it's gone
		const uint64_t since = UInt64Or(pending["since_ms"], 0), now = NowMs();
		const uint64_t start = UInt64Or(pending["old_start"], 0);
		if (start && now >= since + kForceQuitMs && now < since + kTakeOverMs) {
			const uint32_t oldPid = PidOf(pending["old_pid"]);
			static std::atomic<uint64_t> loggedStart{ 0 }; // a process not yet reaped is ended again: logged once
			if (SharedStore::KillProcess(oldPid, start) && loggedStart.exchange(start) != start)
				OOVR_LOGF("The scene application (pid %u) didn't quit for the launch of %s: ended", oldPid, StringOr(pending["app_key"]).c_str());
		}
		return;
	}
	std::string key;
	uint32_t oldPid = 0;
	bool dropped = false;
	store.Update(StoreDoc::Scene, [&](Json::Value& scene) {
		if (!scene.isObject() || !scene["transition"].isObject())
			return;
		Json::Value& tr = scene["transition"];
		if (!tr["state"].isInt() || tr["state"].asInt() != VRApplicationTransition_OldAppQuitSent || !BoolOr(tr["launch"], false))
			return;
		oldPid = PidOf(tr["old_pid"]);
		if (oldPid == oc_current_pid() || SharedStore::IsSameProcessAlive(oldPid, UInt64Or(tr["old_start"], 0)))
			return;
		// Asked for too long ago: dropped, not launched
		if (NowMs() >= UInt64Or(tr["since_ms"], 0) + kTakeOverMs) {
			scene.removeMember("transition");
			dropped = true;
			return;
		}
		key = StringOr(tr["app_key"]);
		tr["state"] = VRApplicationTransition_NewAppLaunched;
		tr["since_ms"] = (Json::UInt64)NowMs();
	});
	if (dropped)
		AnnounceTransitionAborted();
	if (!key.empty())
		LaunchForTransition(store, key, oldPid);
}

void BaseApplications::AddThisProcess(SharedStore& store)
{
	TakeOverPendingLaunch(store);
	const uint32_t pid = oc_current_pid();
	const std::string name = std::to_string(pid);
	const uint64_t start = SharedStore::OwnStartTime();
	// Listed already, by IdentifyApplication or an earlier init: nothing to write
	const Json::Value listedStart = store.ReadAt(StoreDoc::Applications, { "pid_starts", name });
	if (store.ReadAt(StoreDoc::Applications, { "pids", name }).isString() && (!listedStart.isUInt64() || !start || listedStart.asUInt64() == start))
		return;
	const std::string key = CurrentProcessAppKey(store);
	UpdateApplications(store, [&](Json::Value& doc) {
		// IdentifyApplication's entry for this process stays as it is
		const Json::Value starts = doc.get("pid_starts", Json::Value());
		const bool listed = doc["pids"].isObject() && doc["pids"].isMember(name)
		    && (!starts.isObject() || UInt64Or(starts[name], 0) == start || !start);
		const bool automatic = doc["pid_auto"].isObject() && BoolOr(doc["pid_auto"][name], false);
		if (listed && !automatic)
			return;
		RecordProcess(doc, pid, key);
		doc["pid_auto"][name] = true;
	});
}

void BaseApplications::AbortTransition()
{
	bool aborted = false;
	Store().Update(StoreDoc::Scene, [&](Json::Value& scene) {
		if (scene.isObject() && scene.isMember("transition")) {
			aborted = LiveTransition(scene).state != VRApplicationTransition_None;
			scene.removeMember("transition");
		}
	});
	if (aborted)
		AnnounceTransitionAborted();
}

EVRApplicationError BaseApplications::AskSceneApplicationToQuit(const std::string& appKey, bool launchAfter, bool* markedStarting)
{
	SharedStore& store = Store();
	TakeOverPendingLaunch(store);
	if (markedStarting)
		*markedStarting = false;
	// Nothing running and nothing starting: no store write, unless appKey is to be the starting application
	const Json::Value current = store.Read(StoreDoc::Scene);
	if (!markedStarting && !LiveScenePid(current) && LiveTransition(current).state == VRApplicationTransition_None)
		return VRApplicationError_None;

	const uint32_t self = oc_current_pid();
	const std::string selfKey = CurrentProcessAppKey(store);
	EVRApplicationError result = VRApplicationError_None;
	uint32_t scenePid = 0;
	bool post = false;
	// Decided and written at once, so two programs asking together don't both pass
	const bool written = store.Update(StoreDoc::Scene, [&](Json::Value& doc) {
		if (!doc.isObject())
			doc = Json::Value(Json::objectValue);
		const Transition t = LiveTransition(doc);
		if (t.state == VRApplicationTransition_NewAppLaunched) {
			result = t.appKey == appKey ? VRApplicationError_ApplicationAlreadyStarting : VRApplicationError_LaunchInProgress;
			return;
		}
		// The scene application with a session (this program's too): one that has none yet isn't waited for
		scenePid = LiveScenePid(doc);
		if (!scenePid) {
			if (markedStarting && t.state == VRApplicationTransition_None) {
				// No old_pid: a launch with none running, over when another program becomes the scene application
				Json::Value tr(Json::objectValue);
				tr["state"] = VRApplicationTransition_NewAppLaunched;
				tr["app_key"] = appKey;
				tr["since_ms"] = (Json::UInt64)NowMs();
				doc["transition"] = tr;
				*markedStarting = true;
			}
			return;
		}
		if (t.state == VRApplicationTransition_OldAppQuitSent && t.appKey != appKey) {
			result = VRApplicationError_LaunchInProgress;
			return;
		}
		// The application that is the scene application already
		if ((scenePid == self ? selfKey : StringOr(doc["app_key"])) == appKey)
			return;
		const bool sent = t.state == VRApplicationTransition_OldAppQuitSent && t.oldPid == scenePid;
		// Asked again: the time is still the first ask's
		const uint64_t since = sent ? UInt64Or(doc["transition"]["since_ms"], NowMs()) : NowMs();
		Json::Value tr(Json::objectValue);
		tr["state"] = VRApplicationTransition_OldAppQuitSent;
		tr["app_key"] = appKey;
		tr["old_pid"] = scenePid;
		tr["old_start"] = (Json::UInt64)(scenePid == self ? SharedStore::OwnStartTime() : SharedStore::ProcessStartTime(scenePid));
		tr["launch"] = launchAfter || (sent && t.launch);
		tr["since_ms"] = (Json::UInt64)since;
		doc["transition"] = tr;
		result = VRApplicationError_OldApplicationQuitting;
		post = !sent;
	});
	if (!written) {
		// Nothing running or starting when asked: launched without being the starting one, as before
		if (markedStarting && !LiveScenePid(current) && LiveTransition(current).state == VRApplicationTransition_None)
			return VRApplicationError_None;
		return VRApplicationError_IPCFailed;
	}
	if (markedStarting && *markedStarting)
		OcPostEvent(vr::VREvent_SceneApplicationStateChanged, vr::k_unTrackedDeviceIndexInvalid, vr::VREvent_Data_t{}, true);
	if (post) {
		// To the scene application alone: every other program carries on
		vr::VREvent_Data_t data{};
		data.process.pid = scenePid;
		if (!OcPostEventTo(store, scenePid, vr::VREvent_Quit, vr::k_unTrackedDeviceIndexInvalid, data)) {
			// Not asked after all: nothing to wait for (a transition for another application or scene app stays)
			store.Update(StoreDoc::Scene, [&](Json::Value& doc) {
				if (doc.isObject() && PidOf(doc["transition"]["old_pid"]) == scenePid && StringOr(doc["transition"]["app_key"]) == appKey)
					doc.removeMember("transition");
			});
			return VRApplicationError_IPCFailed;
		}
		data.process.oldPid = scenePid;
		data.process.pid = 0;
		OcPostEvent(kTransitionStarted, vr::k_unTrackedDeviceIndexInvalid, data, true);
		// The scene application is quitting
		OcPostEvent(vr::VREvent_SceneApplicationStateChanged, vr::k_unTrackedDeviceIndexInvalid, vr::VREvent_Data_t{}, true);
	}
	return result;
}

void BaseApplications::SetSceneDrawn()
{
	SetSceneDrawn(SharedStore::Get());
}

void BaseApplications::SetSceneDrawn(SharedStore& store)
{
	const uint32_t pid = oc_current_pid();
	vr::VREvent_Data_t data{};
	data.process.pid = pid;
	if (!sceneDrawn.exchange(true))
		OcPostEvent(vr::VREvent_SceneApplicationStateChanged, vr::k_unTrackedDeviceIndexInvalid, data, false);
	// Recorded and told to the other programs, if this one is still the scene application. Called in the game's frame:
	// the store isn't waited for, and what doesn't get its lock is tried again at the next call.
	if (sceneDrawnStored)
		return;
	const bool written = store.Update(
	    StoreDoc::Scene, [&](Json::Value& scene) {
		    if (scene.isObject() && PidOf(scene["pid"]) == pid)
			    scene["drawn"] = true;
	    },
	    SharedStore::Wait::Never);
	if (written && store.PostEvent(vr::VREvent_SceneApplicationStateChanged, OcPackEvent(vr::k_unTrackedDeviceIndexInvalid, data), SharedStore::Wait::Never))
		sceneDrawnStored = true;
}

// Used by Viveport
EVRApplicationError BaseApplications::AddApplicationManifest(const char* pchApplicationManifestFullPath, bool bTemporary)
{
	if (!pchApplicationManifestFullPath || !pchApplicationManifestFullPath[0])
		return VRApplicationError_InvalidManifest;

	std::string path = ManifestKey(pchApplicationManifestFullPath);
	std::vector<std::string> warnings;
	std::vector<AppManifestEntry> apps = ParseAppManifest(PathFromUtf8(path), warnings);
	for (const std::string& w : warnings)
		OOVR_LOGF("Application manifest: %s", w.c_str());
	if (apps.empty())
		return VRApplicationError_InvalidManifest;

	// A key another manifest already has; replaced if that manifest's file is gone (the game moved, say)
	std::set<std::string> ours, stale;
	for (const AppManifestEntry& e : apps)
		ours.insert(e.appKey);
	for (const AppManifestEntry& e : InstalledApplications(Store())) {
		std::string other = PathToUtf8(e.manifestPath);
		if (!ours.count(e.appKey) || other == path)
			continue;
		std::error_code ec;
		// The same file by another spelling (case, or an 8.3 name, on Windows): this one replaces it
		if (std::filesystem::equivalent(e.manifestPath, PathFromUtf8(path), ec)) {
			stale.insert(other);
			continue;
		}
		if (!std::filesystem::exists(e.manifestPath, ec) && !ec) {
			OOVR_LOGF("Application manifest %s: %s replaces the one from %s, which is gone", path.c_str(), e.appKey.c_str(), other.c_str());
			stale.insert(other);
			continue;
		}
		OOVR_LOGF("Application manifest %s: %s is already installed from %s", path.c_str(), e.appKey.c_str(), other.c_str());
		return VRApplicationError_AppKeyAlreadyExists;
	}

	Json::Value list(Json::arrayValue);
	for (const AppManifestEntry& e : apps)
		list.append(e.ToJson());
	uint32_t pid = oc_current_pid();
	bool written = UpdateApplications(Store(), [&](Json::Value& doc) {
		for (const std::string& gone : stale)
			doc["manifests"].removeMember(gone);
		Json::Value& m = doc["manifests"][path];
		m["temporary"] = bTemporary;
		m["owner_pid"] = pid;
		m["apps"] = list;
	});
	OOVR_LOGF("Added application manifest %s (%d applications, temporary=%d)", path.c_str(), (int)apps.size(), bTemporary);
	// The store not written only loses the registry: answered as before it was kept
	if (!written)
		OOVR_LOG("Application manifest not kept: the store can't be written");
	else
		PostApplicationListUpdated();
	return VRApplicationError_None;
}

EVRApplicationError BaseApplications::RemoveApplicationManifest(const char* pchApplicationManifestFullPath)
{
	if (!pchApplicationManifestFullPath || !pchApplicationManifestFullPath[0])
		return VRApplicationError_InvalidManifest;
	std::string path = ManifestKey(pchApplicationManifestFullPath);
	bool existed = false;
	bool written = UpdateApplications(Store(), [&](Json::Value& doc) {
		if (doc.isMember("manifests") && doc["manifests"].isObject()) {
			existed = doc["manifests"].isMember(path);
			doc["manifests"].removeMember(path);
		}
	});
	if (written && existed)
		PostApplicationListUpdated();
	return written ? VRApplicationError_None : VRApplicationError_IPCFailed;
}

bool BaseApplications::IsApplicationInstalled(const char* pchAppKey)
{
	return Find(pchAppKey).has_value();
}
uint32_t BaseApplications::GetApplicationCount()
{
	return (uint32_t)InstalledApplications(Store()).size();
}
EVRApplicationError BaseApplications::GetApplicationKeyByIndex(uint32_t unApplicationIndex, VR_OUT_STRING() char* pchAppKeyBuffer, uint32_t unAppKeyBufferLen)
{
	Clear(pchAppKeyBuffer, unAppKeyBufferLen);
	std::vector<AppManifestEntry> apps = InstalledApplications(Store());
	if (unApplicationIndex >= apps.size())
		return VRApplicationError_InvalidIndex;
	return CopyOut(apps[unApplicationIndex].appKey, pchAppKeyBuffer, unAppKeyBufferLen) ? VRApplicationError_None
	                                                                                     : VRApplicationError_BufferTooSmall;
}
EVRApplicationError BaseApplications::GetApplicationKeyByProcessId(uint32_t unProcessId, VR_OUT_STRING() char* pchAppKeyBuffer, uint32_t unAppKeyBufferLen)
{
	Clear(pchAppKeyBuffer, unAppKeyBufferLen);
	std::string key;
	if (unProcessId == oc_current_pid()) {
		key = CurrentProcessAppKey(Store());
	} else if (SharedStore::IsProcessAlive(unProcessId)) {
		Json::Value doc = Store().Read(StoreDoc::Applications);
		std::string pid = std::to_string(unProcessId);
		if (doc["pids"].isObject() && doc["pids"][pid].isString() && ListedProcessAlive(doc, pid))
			key = doc["pids"][pid].asString();
		Json::Value scene = Store().Read(StoreDoc::Scene);
		if (key.empty() && PidOf(scene["pid"]) == unProcessId)
			key = StringOr(scene["app_key"]);
	}
	if (key.empty())
		return VRApplicationError_UnknownApplication;
	return CopyOut(key, pchAppKeyBuffer, unAppKeyBufferLen) ? VRApplicationError_None : VRApplicationError_BufferTooSmall;
}

EVRApplicationError BaseApplications::Launch(const AppManifestEntry& app, const std::string& extraArguments, uint32_t* startedPid)
{
	if (app.isTemplate)
		return VRApplicationError_IsTemplate;
	if (WindowsBinaryRefused(app))
		return VRApplicationError_LaunchFailed;

	std::optional<uint32_t> pid;
	if (app.launchType == "url") {
		if (!OpenUrl(app.url))
			return VRApplicationError_LaunchFailed;
	} else {
		std::string arguments = app.arguments;
		if (!extraArguments.empty())
			arguments += (arguments.empty() ? "" : " ") + extraArguments;
		pid = LaunchProcess(app.ResolvedBinary(), arguments, app.ResolvedWorkingDirectory());
		if (!pid)
			return VRApplicationError_LaunchFailed;
	}

	if (pid && startedPid)
		*startedPid = *pid;
	OOVR_LOGF("Launched application %s", app.appKey.c_str());
	uint64_t now = UnixSeconds();
	UpdateApplications(Store(), [&](Json::Value& doc) {
		doc["last_launch"][app.appKey] = (Json::UInt64)now;
		if (pid)
			RecordProcess(doc, *pid, app.appKey);
	});
	return VRApplicationError_None;
}

EVRApplicationError BaseApplications::LaunchApplication(const char* pchAppKey)
{
	std::optional<AppManifestEntry> app = Find(pchAppKey);
	if (!app)
		return VRApplicationError_UnknownApplication;
	if (app->isDashboardOverlay)
		return VRApplicationError_InvalidApplication; // LaunchDashboardOverlay's
	// One copy of an application at a time
	if (app->launchType != "url" && GetApplicationProcessId(pchAppKey) != 0)
		return VRApplicationError_ApplicationAlreadyRunning;
	if (!app->isTemplate && WindowsBinaryRefused(*app))
		return VRApplicationError_LaunchFailed;
	// Another scene application is asked to quit, and this one starts when it has
	if (IsSceneApplication(*app)) {
		// With none running, it's the starting application until its session starts (Starting, and named by
		// GetStartingApplication), as SteamVR answers
		bool starting = false;
		EVRApplicationError error = AskSceneApplicationToQuit(app->appKey, true, &starting);
		if (error == VRApplicationError_OldApplicationQuitting)
			return VRApplicationError_None;
		if (error != VRApplicationError_None)
			return error;
		if (starting)
			return LaunchForTransition(Store(), app->appKey, 0);
	}
	return Launch(*app, "");
}
EVRApplicationError BaseApplications::LaunchTemplateApplication(const char* pchTemplateAppKey, const char* pchNewAppKey, VR_ARRAY_COUNT(unKeys) const AppOverrideKeys_t* pKeys, uint32_t unKeys)
{
	std::optional<AppManifestEntry> templ = Find(pchTemplateAppKey);
	if (!templ)
		return VRApplicationError_UnknownApplication;
	if (!templ->isTemplate)
		return VRApplicationError_InvalidApplication;
	if (!pchNewAppKey || !pchNewAppKey[0])
		return VRApplicationError_InvalidParameter;
	if (Find(pchNewAppKey))
		return VRApplicationError_AppKeyAlreadyExists;

	// The template with the overridden keys (named as in a manifest), as a temporary application, kept while the program started for it runs
	Json::Value app = templ->ToJson();
	app["app_key"] = pchNewAppKey;
	app["is_template"] = false;
	for (uint32_t i = 0; pKeys && i < unKeys; i++) {
		if (!pKeys[i].pchKey || !pKeys[i].pchValue)
			continue;
		std::string key = pKeys[i].pchKey;
		if (key == "binary_path_windows" || key == "binary_path_linux")
			key = "binary_path";
		if (app.isMember(key) && app[key].isString())
			app[key] = pKeys[i].pchValue;
	}
	std::string path = "template:" + std::string(pchNewAppKey);
	uint32_t pid = oc_current_pid();
	bool written = UpdateApplications(Store(), [&](Json::Value& doc) {
		Json::Value& m = doc["manifests"][path];
		m["temporary"] = true;
		m["owner_pid"] = pid;
		m["apps"] = Json::Value(Json::arrayValue);
		m["apps"].append(app);
	});
	if (written)
		PostApplicationListUpdated();
	uint32_t started = 0;
	EVRApplicationError error = Launch(AppManifestEntry::FromJson(app), "", &started);
	if (error == VRApplicationError_None && started) {
		// The instance lasts as long as the program started for it, not the one that asked
		UpdateApplications(Store(), [&](Json::Value& doc) {
			if (doc["manifests"].isMember(path))
				doc["manifests"][path]["owner_pid"] = started;
		});
	}
	return error;
}
EVRApplicationError BaseApplications::LaunchApplicationFromMimeType(const char* pchMimeType, const char* pchArgs)
{
	char key[256];
	if (!pchMimeType || !GetDefaultApplicationForMimeType(pchMimeType, key, sizeof(key)))
		return VRApplicationError_UnknownApplication;
	std::optional<AppManifestEntry> app = Find(key);
	if (!app)
		return VRApplicationError_UnknownApplication;
	const std::string args = pchArgs ? pchArgs : "";

	// Running already: the arguments go to it, by a handle it reads them with
	const uint32_t running = app->launchType != "url" ? GetApplicationProcessId(key) : 0;
	if (!running)
		return Launch(*app, args);
	uint32_t handle = 0;
	bool written = UpdateApplications(Store(), [&](Json::Value& doc) {
		handle = doc["next_args_handle"].isUInt() && doc["next_args_handle"].asUInt() ? doc["next_args_handle"].asUInt() : 1;
		doc["next_args_handle"] = handle == UINT32_MAX ? 1u : handle + 1;
		Json::Value& list = doc["launch_args"];
		if (!list.isObject())
			list = Json::Value(Json::objectValue);
		while (list.size() >= kMaxLaunchArgs) {
			// The oldest handle goes
			std::vector<std::string> names = list.getMemberNames();
			auto oldest = std::min_element(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return std::strtoul(a.c_str(), nullptr, 10) < std::strtoul(b.c_str(), nullptr, 10); });
			list.removeMember(*oldest);
		}
		Json::Value entry(Json::objectValue);
		entry["pid"] = running;
		entry["args"] = args;
		list[std::to_string(handle)] = entry;
	});
	if (!written)
		return VRApplicationError_IPCFailed;
	vr::VREvent_Data_t data{};
	data.applicationLaunch.pid = running;
	data.applicationLaunch.unArgsHandle = handle;
	if (!OcPostEventTo(Store(), running, vr::VREvent_ApplicationMimeTypeLoad, vr::k_unTrackedDeviceIndexInvalid, data))
		return VRApplicationError_IPCFailed;
	return VRApplicationError_None;
}
EVRApplicationError BaseApplications::LaunchDashboardOverlay(const char* pchAppKey)
{
	std::optional<AppManifestEntry> app = Find(pchAppKey);
	if (!app)
		return VRApplicationError_UnknownApplication;
	if (!app->isDashboardOverlay)
		return VRApplicationError_InvalidApplication;
	// Already running: nothing to do
	if (GetApplicationProcessId(pchAppKey) != 0)
		return VRApplicationError_None;
	return Launch(*app, "");
}
bool BaseApplications::CancelApplicationLaunch(const char* pchAppKey)
{
	// A launch is started at once, so there's never one waiting to be cancelled
	return false;
}
EVRApplicationError BaseApplications::IdentifyApplication(uint32_t unProcessId, const char* pchAppKey)
{
	// Any key, also one without a manifest here: SteamVR knows steam.app.* keys from Steam
	if (!pchAppKey || !pchAppKey[0])
		return VRApplicationError_UnknownApplication;
	uint32_t pid = unProcessId ? unProcessId : oc_current_pid();
	std::string key = pchAppKey;
	// As AddApplicationManifest: not kept if the store can't be written, answered as before it was
	UpdateApplications(Store(), [&](Json::Value& doc) { RecordProcess(doc, pid, key); });
	return VRApplicationError_None;
}
uint32_t BaseApplications::GetApplicationProcessId(const char* pchAppKey)
{
	if (!pchAppKey || !pchAppKey[0])
		return 0;
	std::string key = pchAppKey;
	if (CurrentProcessAppKey(Store()) == key)
		return oc_current_pid();
	Json::Value doc = Store().Read(StoreDoc::Applications);
	if (doc["pids"].isObject()) {
		for (const std::string& p : doc["pids"].getMemberNames()) {
			uint32_t pid = ParsePid(p);
			if (StringOr(doc["pids"][p]) == key && SharedStore::IsProcessAlive(pid) && ListedProcessAlive(doc, p))
				return pid;
		}
	}
	Json::Value scene = Store().Read(StoreDoc::Scene);
	if (StringOr(scene["app_key"]) == key && SharedStore::IsSameProcessAlive(PidOf(scene["pid"]), UInt64Or(scene["start"], 0)))
		return PidOf(scene["pid"]);
	return 0;
}
const char* BaseApplications::GetApplicationsErrorNameFromEnum(EVRApplicationError error)
{
	// As SteamVR answers
	return OC_EnumNameAsSteamVR("EVRApplicationError", (int32_t)error, "Unknown VRApplicationsError (%d)");
}
uint32_t BaseApplications::GetApplicationPropertyString(const char* pchAppKey, EVRApplicationProperty eProperty, VR_OUT_STRING() char* pchPropertyValueBuffer, uint32_t unPropertyValueBufferLen, EVRApplicationError* peError)
{
	Clear(pchPropertyValueBuffer, unPropertyValueBufferLen);
	auto fail = [&](EVRApplicationError e) -> uint32_t {
		if (peError)
			*peError = e;
		return 1; // SteamVR's: an empty string's size
	};
	std::optional<AppManifestEntry> app = Find(pchAppKey);
	if (!app)
		return fail(VRApplicationError_UnknownApplication);

	std::string value;
	switch (eProperty) {
	case VRApplicationProperty_Name_String:
		value = app->nameEnUs.empty() ? app->appKey : app->nameEnUs;
		break;
	case VRApplicationProperty_LaunchType_String:
		value = app->launchType;
		break;
	case VRApplicationProperty_WorkingDirectory_String:
		value = PathToUtf8(app->ResolvedWorkingDirectory());
		break;
	case VRApplicationProperty_BinaryPath_String:
		value = PathToUtf8(app->ResolvedBinary());
		break;
	case VRApplicationProperty_Arguments_String:
		value = app->arguments;
		break;
	case VRApplicationProperty_URL_String:
		value = app->url;
		break;
	case VRApplicationProperty_Description_String:
		value = app->descriptionEnUs;
		break;
	case VRApplicationProperty_NewsURL_String:
		value = app->newsUrl;
		break;
	case VRApplicationProperty_ImagePath_String:
		value = PathToUtf8(app->Resolved(app->imagePath));
		break;
	case VRApplicationProperty_Source_String:
		value = app->source;
		break;
	case VRApplicationProperty_ActionManifestURL_String:
		value = PathToUtf8(app->Resolved(app->actionManifestPath));
		break;
	case VRApplicationProperty_ImagePathCapsule_String:
		value = PathToUtf8(app->Resolved(app->imagePathCapsule));
		break;
	default:
		return fail(VRApplicationError_UnknownProperty);
	}
	if (value.empty())
		return fail(VRApplicationError_PropertyNotSet);

	bool fits = CopyOut(value, pchPropertyValueBuffer, unPropertyValueBufferLen);
	if (peError)
		*peError = fits ? VRApplicationError_None : VRApplicationError_BufferTooSmall;
	if (!fits)
		Clear(pchPropertyValueBuffer, unPropertyValueBufferLen);
	return (uint32_t)value.size() + 1;
}
bool BaseApplications::GetApplicationPropertyBool(const char* pchAppKey, EVRApplicationProperty eProperty, EVRApplicationError* peError)
{
	auto answer = [&](EVRApplicationError e, bool value) {
		if (peError)
			*peError = e;
		return value;
	};
	std::optional<AppManifestEntry> app = Find(pchAppKey);
	if (!app)
		return answer(VRApplicationError_UnknownApplication, false);
	switch (eProperty) {
	case VRApplicationProperty_IsDashboardOverlay_Bool:
		return answer(VRApplicationError_None, app->isDashboardOverlay);
	case VRApplicationProperty_IsTemplate_Bool:
		return answer(VRApplicationError_None, app->isTemplate);
	case VRApplicationProperty_IsInstanced_Bool:
		return answer(VRApplicationError_None, app->isInstanced);
	case VRApplicationProperty_IsInternal_Bool:
		return answer(VRApplicationError_None, app->isInternal);
	case VRApplicationProperty_WantsCompositorPauseInStandby_Bool:
		return answer(VRApplicationError_None, app->wantsCompositorPauseInStandby);
	case VRApplicationProperty_IsHidden_Bool:
		return answer(VRApplicationError_None, app->isHidden);
	default:
		return answer(VRApplicationError_UnknownProperty, false);
	}
}
uint64_t BaseApplications::GetApplicationPropertyUint64(const char* pchAppKey, EVRApplicationProperty eProperty, EVRApplicationError* peError)
{
	auto answer = [&](EVRApplicationError e, uint64_t value) {
		if (peError)
			*peError = e;
		return value;
	};
	std::optional<AppManifestEntry> app = Find(pchAppKey);
	if (!app)
		return answer(VRApplicationError_UnknownApplication, 0);
	if (eProperty != VRApplicationProperty_LastLaunchTime_Uint64)
		return answer(VRApplicationError_UnknownProperty, 0);
	Json::Value doc = Store().Read(StoreDoc::Applications);
	return answer(VRApplicationError_None, doc["last_launch"].isObject() ? UInt64Or(doc["last_launch"][app->appKey], 0) : 0);
}
EVRApplicationError BaseApplications::SetApplicationAutoLaunch(const char* pchAppKey, bool bAutoLaunch)
{
	std::optional<AppManifestEntry> app = Find(pchAppKey);
	if (!app)
		return VRApplicationError_UnknownApplication;
	// Only a dashboard overlay is started with the dashboard
	if (!app->isDashboardOverlay)
		return VRApplicationError_InvalidApplication;
	std::string key = pchAppKey;
	bool written = UpdateApplications(Store(), [&](Json::Value& doc) { doc["autolaunch"][key] = bAutoLaunch; });
	return written ? VRApplicationError_None : VRApplicationError_IPCFailed;
}
bool BaseApplications::GetApplicationAutoLaunch(const char* pchAppKey)
{
	if (!pchAppKey)
		return false;
	Json::Value doc = Store().Read(StoreDoc::Applications);
	return doc["autolaunch"].isObject() && BoolOr(doc["autolaunch"][pchAppKey], false);
}
EVRApplicationError BaseApplications::SetDefaultApplicationForMimeType(const char* pchAppKey, const char* pchMimeType)
{
	if (!Find(pchAppKey))
		return VRApplicationError_UnknownApplication;
	if (!pchMimeType || !pchMimeType[0])
		return VRApplicationError_InvalidParameter;
	std::string key = pchAppKey, type = pchMimeType;
	bool written = UpdateApplications(Store(), [&](Json::Value& doc) { doc["mime_defaults"][type] = key; });
	return written ? VRApplicationError_None : VRApplicationError_IPCFailed;
}
bool BaseApplications::GetDefaultApplicationForMimeType(const char* pchMimeType, VR_OUT_STRING() char* pchAppKeyBuffer, uint32_t unAppKeyBufferLen)
{
	Clear(pchAppKeyBuffer, unAppKeyBufferLen);
	if (!pchMimeType)
		return false;
	std::string type = pchMimeType;
	Json::Value doc = Store().Read(StoreDoc::Applications);
	std::vector<AppManifestEntry> apps = InstalledApplications(Store());

	// The default set, if it's still installed; else the first application that opens it
	std::string key = doc["mime_defaults"].isObject() ? StringOr(doc["mime_defaults"][type]) : std::string();
	bool installed = false;
	for (const AppManifestEntry& e : apps)
		installed |= e.appKey == key;
	if (!installed) {
		key.clear();
		for (const AppManifestEntry& e : apps) {
			if (std::find(e.mimeTypes.begin(), e.mimeTypes.end(), type) != e.mimeTypes.end()) {
				key = e.appKey;
				break;
			}
		}
	}
	if (key.empty())
		return false;
	return CopyOut(key, pchAppKeyBuffer, unAppKeyBufferLen);
}
bool BaseApplications::GetApplicationSupportedMimeTypes(const char* pchAppKey, VR_OUT_STRING() char* pchMimeTypesBuffer, uint32_t unMimeTypesBuffer)
{
	Clear(pchMimeTypesBuffer, unMimeTypesBuffer);
	std::optional<AppManifestEntry> app = Find(pchAppKey);
	if (!app)
		return false;
	std::vector<std::string> types = MimeTypesOf(*app, Store().Read(StoreDoc::Applications));
	if (types.empty())
		return false;
	return CopyOut(Join(types), pchMimeTypesBuffer, unMimeTypesBuffer);
}
uint32_t BaseApplications::GetApplicationsThatSupportMimeType(const char* pchMimeType, VR_OUT_STRING() char* pchAppKeysThatSupportBuffer, uint32_t unAppKeysThatSupportBuffer)
{
	Clear(pchAppKeysThatSupportBuffer, unAppKeysThatSupportBuffer);
	if (!pchMimeType)
		return 0;
	Json::Value doc = Store().Read(StoreDoc::Applications);
	std::vector<std::string> keys;
	for (const AppManifestEntry& e : InstalledApplications(Store())) {
		std::vector<std::string> types = MimeTypesOf(e, doc);
		if (std::find(types.begin(), types.end(), pchMimeType) != types.end())
			keys.push_back(e.appKey);
	}
	if (keys.empty())
		return 0;
	std::string list = Join(keys);
	CopyOut(list, pchAppKeysThatSupportBuffer, unAppKeysThatSupportBuffer);
	return (uint32_t)list.size() + 1;
}
uint32_t BaseApplications::GetApplicationLaunchArguments(uint32_t unHandle, VR_OUT_STRING() char* pchArgs, uint32_t unArgs)
{
	Clear(pchArgs, unArgs);
	// The arguments of a VREvent_ApplicationMimeTypeLoad this program got
	Json::Value entry = Store().ReadAt(StoreDoc::Applications, { "launch_args", std::to_string(unHandle) });
	if (!entry.isObject() || PidOf(entry["pid"]) != oc_current_pid() || !entry["args"].isString())
		return 0;
	const std::string args = entry["args"].asString();
	CopyOut(args, pchArgs, unArgs);
	return (uint32_t)args.size() + 1;
}
EVRApplicationError BaseApplications::GetStartingApplication(VR_OUT_STRING() char* pchAppKeyBuffer, uint32_t unAppKeyBufferLen)
{
	Clear(pchAppKeyBuffer, unAppKeyBufferLen);
	// The application waiting for the scene application to quit, or launched once it had
	TakeOverPendingLaunch(Store());
	Transition t = LiveTransition(Store().Read(StoreDoc::Scene));
	if (t.state == VRApplicationTransition_None || t.appKey.empty())
		return VRApplicationError_NoApplication;
	return CopyOut(t.appKey, pchAppKeyBuffer, unAppKeyBufferLen) ? VRApplicationError_None : VRApplicationError_BufferTooSmall;
}
BaseApplications::EVRApplicationTransitionState BaseApplications::GetTransitionState()
{
	TakeOverPendingLaunch(Store());
	return LiveTransition(Store().Read(StoreDoc::Scene)).state;
}
BaseApplications::EVRSceneApplicationState BaseApplications::GetSceneApplicationState()
{
	// A scene application, in this program or another: Quitting once asked to quit for a transition, else Running once
	// it has drawn and Waiting until then. Without one, Starting while a transition launches the next.
	Json::Value scene = Store().Read(StoreDoc::Scene);
	const Transition t = LiveTransition(scene);
	const uint32_t pid = GetCurrentSceneProcessId();
	if (pid == 0)
		return t.state == VRApplicationTransition_NewAppLaunched ? EVRSceneApplicationState_Starting : EVRSceneApplicationState_None;
	if (t.state == VRApplicationTransition_OldAppQuitSent && t.oldPid == pid)
		return EVRSceneApplicationState_Quitting;
	bool drawn = sceneDrawn;
	if (pid != oc_current_pid()) {
		const Json::Value& stored = scene["drawn"];
		drawn = PidOf(scene["pid"]) == pid && stored.isBool() && stored.asBool();
	}
	return drawn ? EVRSceneApplicationState_Running : EVRSceneApplicationState_Waiting;
}
EVRApplicationError BaseApplications::PerformApplicationPrelaunchCheck(const char* pchAppKey)
{
	std::optional<AppManifestEntry> app = Find(pchAppKey);
	if (!app)
		return VRApplicationError_UnknownApplication;
	if (!IsSceneApplication(*app))
		return VRApplicationError_None;
	// Another scene application is asked to quit; the caller waits for its VREvent_ProcessQuit and asks again
	return AskSceneApplicationToQuit(app->appKey, false);
}
const char* BaseApplications::GetApplicationsTransitionStateNameFromEnum(EVRApplicationTransitionState state)
{
	// As SteamVR answers
	return OC_EnumNameAsSteamVR("EVRApplicationTransitionState", (int32_t)state, "Unknown EVRApplicationTransitionState");
}
const char* BaseApplications::GetSceneApplicationStateNameFromEnum(BaseApplications::EVRSceneApplicationState state)
{
	// As SteamVR answers
	return OC_EnumNameAsSteamVR("EVRSceneApplicationState", (int32_t)state, "Unknown EVRSceneApplicationState (%d)");
}
bool BaseApplications::IsQuitUserPromptRequested()
{
	return false;
}
EVRApplicationError BaseApplications::LaunchInternalProcess(const char* pchBinaryPath, const char* pchArguments, const char* pchWorkingDirectory)
{
	if (!pchBinaryPath || !pchBinaryPath[0])
		return VRApplicationError_LaunchFailed;
	std::filesystem::path binary = PathFromUtf8(pchBinaryPath);
	// No working folder: the binary's own
	std::filesystem::path dir = pchWorkingDirectory && pchWorkingDirectory[0] ? PathFromUtf8(pchWorkingDirectory) : binary.parent_path();
	OOVR_LOGF("Launching an internal process: %s %s (in %s)", pchBinaryPath, pchArguments ? pchArguments : "", PathToUtf8(dir).c_str());
	std::optional<uint32_t> pid = LaunchProcess(binary, pchArguments ? pchArguments : "", dir);
	if (!pid)
		return VRApplicationError_LaunchFailed;

	// The new process is part of this application
	std::string key = CurrentProcessAppKey(Store());
	UpdateApplications(Store(), [&](Json::Value& doc) { RecordProcess(doc, *pid, key); });
	return VRApplicationError_None;
}
uint32_t BaseApplications::GetCurrentSceneProcessId()
{
	// This program, if it's a scene application (before its session starts, too): the Scene document may still name
	// an earlier one
	if (BaseClientCore::appType == VRApplication_Scene)
		return oc_current_pid();
	Json::Value scene = Store().Read(StoreDoc::Scene);
	uint32_t pid = PidOf(scene["pid"]);
	return pid && SharedStore::IsProcessAlive(pid) ? pid : 0;
}

EVRApplicationError BaseApplications::RegisterSubprocess(uint32_t nPid)
{
	// The subprocess is part of this application
	if (nPid && SharedStore::IsProcessAlive(nPid)) {
		std::string key = CurrentProcessAppKey(Store());
		UpdateApplications(Store(), [&](Json::Value& doc) { RecordProcess(doc, nPid, key); });
	}
	return VRApplicationError_None;
}

// IVRApplications_001's home application: none is set (NoApplication, as SteamVR answers), and none can be
// (InvalidApplication, SteamVR's answer to any key)
EVRApplicationError BaseApplications::GetHomeApplication(char* pchAppKeyBuffer, uint32_t unAppKeyBufferLen)
{
	Clear(pchAppKeyBuffer, unAppKeyBufferLen);
	return VRApplicationError_NoApplication;
}

EVRApplicationError BaseApplications::SetHomeApplication(const char* pchAppKey)
{
	OC_UNSUPPORTED(VRApplicationError_InvalidApplication, "SetHomeApplication (IVRApplications_001): there's no home "
	                                                      "application on OpenXR");
}
