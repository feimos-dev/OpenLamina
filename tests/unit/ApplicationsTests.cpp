// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

#include "Misc/AppManifest.h"
#include "Misc/Config.h"
#include "Misc/Dashboard.h"
#include "Misc/Process.h"
#include "Misc/ProgramEvents.h"
#include "Misc/SharedStore.h"
#include "Reimpl/BaseApplications.h"
#include "Reimpl/BaseClientCore.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {
// A fresh folder under the system's temporary one (named sub, if given), removed again at the end
struct TempDir {
	fs::path root, path;
	explicit TempDir(const fs::path& sub = {})
	{
		auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		root = fs::temp_directory_path() / ("oc_apps_" + std::to_string(stamp));
		path = sub.empty() ? root : root / sub;
		fs::create_directories(path);
	}
	~TempDir()
	{
		std::error_code ec;
		fs::remove_all(root, ec);
	}
};

fs::path Data(const char* name)
{
	return fs::path(OC_TEST_DATA_DIR) / name;
}

// The SteamAppId environment variable for one test
struct AppId {
	explicit AppId(const char* id) { Set(id); }
	~AppId() { Set(nullptr); }
	static void Set(const char* id)
	{
#ifdef _WIN32
		_putenv_s("SteamAppId", id ? id : "");
#else
		if (id)
			setenv("SteamAppId", id, 1);
		else
			unsetenv("SteamAppId");
#endif
	}
};

// The id of a process that has started and ended
uint32_t ExitedProcessId()
{
#ifdef _WIN32
	wchar_t cmd[] = L"cmd.exe /c exit 0";
	STARTUPINFOW si{};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi{};
	REQUIRE(CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi));
	const DWORD wait = WaitForSingleObject(pi.hProcess, 10000);
	if (wait != WAIT_OBJECT_0)
		TerminateProcess(pi.hProcess, 1);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess); // the last handle: the id is the ended process's no more
	REQUIRE(wait == WAIT_OBJECT_0);
	return (uint32_t)pi.dwProcessId;
#else
	pid_t pid = fork();
	if (pid == 0)
		_exit(0);
	REQUIRE(pid > 0);
	int status = 0;
	waitpid(pid, &status, 0);
	return (uint32_t)pid;
#endif
}

std::string KeyString(BaseApplications& apps, uint32_t index)
{
	char key[128] = "unchanged";
	apps.GetApplicationKeyByIndex(index, key, sizeof(key));
	return key;
}
} // namespace

TEST_CASE("Manifest_parses_fields")
{
	std::vector<std::string> warnings;
	std::vector<AppManifestEntry> apps = ParseAppManifest(Data("overlay_apps.vrmanifest"), warnings);
	REQUIRE(apps.size() == 2);
	CHECK(warnings.size() == 1); // the application without a key

	const AppManifestEntry& overlay = apps[0];
	CHECK(overlay.appKey == "oc.test.overlay");
	CHECK(overlay.launchType == "binary");
	CHECK(overlay.arguments == "--overlay");
	CHECK(overlay.isDashboardOverlay);
	CHECK_FALSE(overlay.isTemplate);
	CHECK(overlay.nameEnUs == "Overlay Tool");
	CHECK(overlay.descriptionEnUs == "A dashboard overlay");
	CHECK(overlay.source == "oc_tests");
	CHECK(overlay.ResolvedBinary() == (fs::absolute(Data("bin")) / "overlay_tool").lexically_normal());
	CHECK(overlay.ResolvedWorkingDirectory() == fs::absolute(Data("bin")).lexically_normal());
	CHECK(overlay.Resolved(overlay.actionManifestPath) == fs::absolute(Data("actions.json")).lexically_normal());

	const AppManifestEntry& templ = apps[1];
	CHECK(templ.launchType == "url");
	CHECK(templ.url == "https://example.invalid/app");
	CHECK(templ.isTemplate);

	// The platform's binary
	std::vector<AppManifestEntry> echo = ParseAppManifest(Data("echo.vrmanifest"), warnings);
	REQUIRE(echo.size() == 1);
#ifdef _WIN32
	CHECK(echo[0].binaryPath == "oc_echo_app.exe");
#else
	CHECK(echo[0].binaryPath == "oc_echo_app");
#endif
	CHECK(echo[0].mimeTypes == std::vector<std::string>{ "vr/oc-echo" });

	// Through the store and back
	AppManifestEntry back = AppManifestEntry::FromJson(overlay.ToJson());
	CHECK(back.appKey == overlay.appKey);
	CHECK(back.ResolvedBinary() == overlay.ResolvedBinary());
	CHECK(back.isDashboardOverlay);

	// No such file, or not a manifest
	warnings.clear();
	CHECK(ParseAppManifest(Data("no_such.vrmanifest"), warnings).empty());
	CHECK(warnings.size() == 1);
	CHECK(ParseAppManifest(Data("steamvr.vrsettings"), warnings).empty());
}

TEST_CASE("Manifest_with_non_ascii_path")
{
	TempDir dir(fs::path(u8"Jürgen_テスト") / u8"Spiele");
	fs::path manifest = dir.path / u8"spiel.vrmanifest";
	fs::copy_file(Data("echo.vrmanifest"), manifest);

	SharedStore store(dir.root / "store");
	BaseApplications apps(store);
	CHECK(apps.AddApplicationManifest(PathToUtf8(manifest).c_str()) == VRApplicationError_None);
	CHECK(apps.IsApplicationInstalled("oc.test.echo"));
	char binary[512];
	BaseApplications::EVRApplicationError error;
	apps.GetApplicationPropertyString("oc.test.echo", VRApplicationProperty_BinaryPath_String, binary, sizeof(binary), &error);
	CHECK(error == VRApplicationError_None);
	CHECK(PathFromUtf8(binary).parent_path() == dir.path.lexically_normal());
}

TEST_CASE("File_url_has_three_slashes")
{
#ifdef _WIN32
	CHECK(FileUrl(PathFromUtf8("C:\\Spiele\\J\xC3\xBCrgen.png")) == "file:///C:/Spiele/J\xC3\xBCrgen.png");
#else
	CHECK(FileUrl(PathFromUtf8("/home/j\xC3\xBCrgen/a.png")) == "file:///home/j\xC3\xBCrgen/a.png");
#endif
}

TEST_CASE("Applications_registry_calls")
{
	TempDir dir;
	SharedStore store(dir.path);
	BaseApplications apps(store);
	BaseApplications::EVRApplicationError error;

	CHECK(apps.GetApplicationCount() == 0);
	CHECK(apps.AddApplicationManifest(PathToUtf8(Data("overlay_apps.vrmanifest")).c_str()) == VRApplicationError_None);
	CHECK(apps.AddApplicationManifest(PathToUtf8(Data("echo.vrmanifest")).c_str()) == VRApplicationError_None);
	CHECK(apps.AddApplicationManifest(PathToUtf8(Data("steamvr.vrsettings")).c_str()) == VRApplicationError_InvalidManifest);
	CHECK(apps.GetApplicationCount() == 3);
	CHECK(KeyString(apps, 0) == "oc.test.echo"); // by key
	CHECK(KeyString(apps, 2) == "oc.test.template");
	char key[4];
	CHECK(apps.GetApplicationKeyByIndex(0, key, sizeof(key)) == VRApplicationError_BufferTooSmall);
	CHECK(apps.GetApplicationKeyByIndex(3, key, sizeof(key)) == VRApplicationError_InvalidIndex);

	// Properties
	char name[64];
	CHECK(apps.GetApplicationPropertyString("oc.test.echo", VRApplicationProperty_Name_String, name, sizeof(name), &error) == 5);
	CHECK(std::string(name) == "Echo");
	CHECK(error == VRApplicationError_None);
	CHECK(apps.GetApplicationPropertyString("oc.test.echo", VRApplicationProperty_Name_String, name, 3, &error) == 5);
	CHECK(error == VRApplicationError_BufferTooSmall);
	apps.GetApplicationPropertyString("oc.test.echo", VRApplicationProperty_URL_String, name, sizeof(name), &error);
	CHECK(error == VRApplicationError_PropertyNotSet);
	CHECK(apps.GetApplicationPropertyBool("oc.test.overlay", VRApplicationProperty_IsDashboardOverlay_Bool, &error));
	CHECK(error == VRApplicationError_None);
	CHECK(apps.GetApplicationPropertyBool("oc.test.template", VRApplicationProperty_IsTemplate_Bool, &error));
	CHECK_FALSE(apps.GetApplicationPropertyBool("oc.none", VRApplicationProperty_IsTemplate_Bool, &error));
	CHECK(error == VRApplicationError_UnknownApplication);
	CHECK(apps.GetApplicationPropertyUint64("oc.test.echo", VRApplicationProperty_LastLaunchTime_Uint64, &error) == 0);

	// The same key from another manifest
	TempDir other;
	fs::path copy = other.path / "copy.vrmanifest";
	fs::copy_file(Data("echo.vrmanifest"), copy);
	CHECK(apps.AddApplicationManifest(PathToUtf8(copy).c_str()) == VRApplicationError_AppKeyAlreadyExists);

	// Removed again
	CHECK(apps.RemoveApplicationManifest(PathToUtf8(Data("overlay_apps.vrmanifest")).c_str()) == VRApplicationError_None);
	CHECK(apps.GetApplicationCount() == 1);
	CHECK_FALSE(apps.IsApplicationInstalled("oc.test.overlay"));

	// Another program sees the same registry
	SharedStore store2(dir.path);
	BaseApplications apps2(store2);
	CHECK(apps2.IsApplicationInstalled("oc.test.echo"));
	CHECK(apps2.IdentifyApplication(0, "oc.test.echo") == VRApplicationError_None);
	CHECK(CurrentProcessAppKey(store) == "oc.test.echo");
	CHECK(apps.IdentifyApplication(0, "") == VRApplicationError_UnknownApplication);
	// A key without a manifest is taken too
	CHECK(apps.IdentifyApplication(0, "steam.app.620980") == VRApplicationError_None);
	CHECK(CurrentProcessAppKey(store) == "steam.app.620980");
}

TEST_CASE("Temporary_manifest_goes_with_its_process")
{
	TempDir dir;
	SharedStore store(dir.path);
	BaseApplications apps(store);

	// Ours, while we run
	CHECK(apps.AddApplicationManifest(PathToUtf8(Data("echo.vrmanifest")).c_str(), true) == VRApplicationError_None);
	CHECK(apps.IsApplicationInstalled("oc.test.echo"));

	// A child's that has exited: gone
	uint32_t dead = ExitedProcessId();
	store.Update(StoreDoc::Applications, [&](Json::Value& doc) {
		for (const std::string& path : doc["manifests"].getMemberNames())
			doc["manifests"][path]["owner_pid"] = dead;
	});
	CHECK_FALSE(apps.IsApplicationInstalled("oc.test.echo"));
	CHECK(apps.GetApplicationCount() == 0);
}

TEST_CASE("Key_from_SteamAppId")
{
	TempDir dir;
	SharedStore store(dir.path);
	{
		AppId id("620980");
		CHECK(CurrentProcessAppKey(store) == "steam.app.620980");
	}
	// Without it, a key of its own from the program's name
	std::string key = CurrentProcessAppKey(store);
	CHECK(key.rfind("system.generated.oc_unit_tests", 0) == 0);
	BaseApplications apps(store);
	char buffer[128];
	CHECK(apps.GetApplicationKeyByProcessId(oc_current_pid(), buffer, sizeof(buffer)) == VRApplicationError_None);
	CHECK(std::string(buffer) == key);
	CHECK(apps.GetApplicationProcessId(key.c_str()) == oc_current_pid());
}

TEST_CASE("Autolaunch_kept")
{
	TempDir dir;
	SharedStore store(dir.path);
	BaseApplications apps(store);
	CHECK(apps.SetApplicationAutoLaunch("oc.test.overlay", true) == VRApplicationError_UnknownApplication);
	apps.AddApplicationManifest(PathToUtf8(Data("overlay_apps.vrmanifest")).c_str());
	CHECK_FALSE(apps.GetApplicationAutoLaunch("oc.test.overlay"));
	CHECK(apps.SetApplicationAutoLaunch("oc.test.overlay", true) == VRApplicationError_None);
	SharedStore store2(dir.path);
	CHECK(BaseApplications(store2).GetApplicationAutoLaunch("oc.test.overlay"));
	CHECK(apps.SetApplicationAutoLaunch("oc.test.overlay", false) == VRApplicationError_None);
	CHECK_FALSE(apps.GetApplicationAutoLaunch("oc.test.overlay"));
}

TEST_CASE("Mime_default_kept")
{
	TempDir dir;
	SharedStore store(dir.path);
	BaseApplications apps(store);
	apps.AddApplicationManifest(PathToUtf8(Data("overlay_apps.vrmanifest")).c_str());
	apps.AddApplicationManifest(PathToUtf8(Data("echo.vrmanifest")).c_str());
	char buffer[128];

	// From the manifest
	CHECK(apps.GetDefaultApplicationForMimeType("vr/oc-echo", buffer, sizeof(buffer)));
	CHECK(std::string(buffer) == "oc.test.echo");
	CHECK(apps.GetApplicationSupportedMimeTypes("oc.test.echo", buffer, sizeof(buffer)));
	CHECK(std::string(buffer) == "vr/oc-echo");
	CHECK_FALSE(apps.GetDefaultApplicationForMimeType("vr/nobody", buffer, sizeof(buffer)));

	// Set, and kept for every program
	CHECK(apps.SetDefaultApplicationForMimeType("oc.test.overlay", "vr/oc-echo") == VRApplicationError_None);
	SharedStore store2(dir.path);
	BaseApplications apps2(store2);
	CHECK(apps2.GetDefaultApplicationForMimeType("vr/oc-echo", buffer, sizeof(buffer)));
	CHECK(std::string(buffer) == "oc.test.overlay");
	CHECK(apps2.GetApplicationSupportedMimeTypes("oc.test.overlay", buffer, sizeof(buffer)));
	CHECK(std::string(buffer) == "vr/oc-echo");
	CHECK(apps2.GetApplicationsThatSupportMimeType("vr/oc-echo", buffer, sizeof(buffer)) == strlen("oc.test.echo,oc.test.overlay") + 1);
	CHECK(std::string(buffer) == "oc.test.echo,oc.test.overlay");
	CHECK(apps.SetDefaultApplicationForMimeType("oc.none", "vr/oc-echo") == VRApplicationError_UnknownApplication);
}

TEST_CASE("Registry_values_of_another_type_read_as_unset")
{
	TempDir dir;
	SharedStore store(dir.path);
	BaseApplications apps(store);
	BaseApplications::EVRApplicationError error;
	apps.AddApplicationManifest(PathToUtf8(Data("echo.vrmanifest")).c_str());
	CHECK(store.Update(StoreDoc::Applications, [](Json::Value& doc) {
		doc["autolaunch"]["oc.test.echo"] = "yes";
		doc["last_launch"]["oc.test.echo"] = "today";
		doc["mime_defaults"]["vr/oc-echo"] = Json::Value(Json::objectValue);
		doc["pids"]["1"] = Json::Value(Json::arrayValue);
	}));
	CHECK(store.Update(StoreDoc::Scene, [](Json::Value& scene) { scene["app_key"] = Json::Value(Json::objectValue); }));

	char buffer[128];
	CHECK_FALSE(apps.GetApplicationAutoLaunch("oc.test.echo"));
	CHECK(apps.GetApplicationPropertyUint64("oc.test.echo", VRApplicationProperty_LastLaunchTime_Uint64, &error) == 0);
	CHECK(error == VRApplicationError_None);
	CHECK(apps.GetDefaultApplicationForMimeType("vr/oc-echo", buffer, sizeof(buffer)));
	CHECK(std::string(buffer) == "oc.test.echo");
	CHECK(apps.GetApplicationSupportedMimeTypes("oc.test.echo", buffer, sizeof(buffer)));
	CHECK(apps.GetApplicationProcessId("oc.none") == 0);
}

TEST_CASE("Launch_unknown_key_errors")
{
	TempDir dir;
	SharedStore store(dir.path);
	BaseApplications apps(store);
	CHECK(apps.LaunchApplication("oc.none") == VRApplicationError_UnknownApplication);
	CHECK(apps.LaunchApplication(nullptr) == VRApplicationError_UnknownApplication);
	CHECK(apps.PerformApplicationPrelaunchCheck("oc.none") == VRApplicationError_UnknownApplication);

	apps.AddApplicationManifest(PathToUtf8(Data("overlay_apps.vrmanifest")).c_str());
	CHECK(apps.LaunchApplication("oc.test.template") == VRApplicationError_IsTemplate);
	CHECK(apps.LaunchApplication("oc.test.overlay") == VRApplicationError_InvalidApplication); // a dashboard overlay
	CHECK(apps.LaunchDashboardOverlay("oc.test.overlay") == VRApplicationError_LaunchFailed); // its binary isn't there
	CHECK(apps.LaunchTemplateApplication("oc.test.overlay", "oc.new", nullptr, 0) == VRApplicationError_InvalidApplication);
	CHECK(apps.PerformApplicationPrelaunchCheck("oc.test.overlay") == VRApplicationError_None);
}

#ifndef _WIN32
// A Windows program can't be started on Linux: refused before it's tried, though the file is there and would run
TEST_CASE("Windows_binary_refused_on_Linux")
{
	TempDir dir;
	SharedStore store(dir.path / "store");
	BaseApplications apps(store);
	fs::path exe = dir.path / "Game.EXE";
	fs::path marker = dir.path / "started";
	std::ofstream(exe) << "#!/bin/sh\ntouch \"$1\"\n";
	fs::permissions(exe, fs::perms::owner_all);
	std::ofstream(dir.path / "game.vrmanifest") << "{ \"applications\" : [ { \"app_key\" : \"oc.test.windows\", "
	                                                  "\"binary_path\" : \"Game.EXE\", \"arguments\" : \""
	                                               << PathToUtf8(marker) << "\" } ] }";
	REQUIRE(apps.AddApplicationManifest(PathToUtf8(dir.path / "game.vrmanifest").c_str(), true) == VRApplicationError_None);
	CHECK(apps.LaunchApplication("oc.test.windows") == VRApplicationError_LaunchFailed);
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	CHECK_FALSE(fs::exists(marker));
	CHECK(apps.GetApplicationProcessId("oc.test.windows") == 0);
}
#endif

TEST_CASE("Command_line_split_as_CommandLineToArgvW")
{
	using V = std::vector<std::string>;
	CHECK(SplitCommandLine("") == V{});
	CHECK(SplitCommandLine("  a\tb  ") == V{ "a", "b" });
	CHECK(SplitCommandLine("\"a b\" c\"d e\"f \"\"") == V{ "a b", "cd ef", "" });
	CHECK(SplitCommandLine("& $HOME ; `x` | > *") == V{ "&", "$HOME", ";", "`x`", "|", ">", "*" });
	CHECK(SplitCommandLine("a\\b \\\\\"c d\" \\\"e") == V{ "a\\b", "\\c d", "\"e" });
	CHECK(SplitCommandLine("\"a \"\"b\"") == V{ "a \"b" });
	for (const std::string& arg : { std::string("plain"), std::string("with space"), std::string("q\"uote"),
	         std::string("trail\\"), std::string("back\\\"slash"), std::string("") })
		CHECK(SplitCommandLine("x " + QuoteArgument(arg) + " y") == V{ "x", arg, "y" });
}

TEST_CASE("Launch_without_a_shell")
{
	TempDir dir("with space");
	// The marker named relative to the working folder; nothing of it read by a shell
	std::optional<uint32_t> pid = LaunchProcess(fs::path(OC_ECHO_APP), "\"marker file.txt\" a & $HOME ;b `c` \"d e\"", dir.path);
	REQUIRE(pid);

	// It ends, and isn't left a zombie that seems to run
	bool ended = false;
	for (int i = 0; i < 50 && !ended; i++) {
		ended = !SharedStore::IsProcessAlive(*pid);
		if (!ended)
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	CHECK(ended);
	std::ifstream in(dir.path / "marker file.txt");
	std::string line;
	std::getline(in, line);
	CHECK(line == "a & $HOME ;b `c` d e");
}

TEST_CASE("Ended_programs_dropped_on_every_update")
{
	TempDir dir;
	SharedStore store(dir.path);
	BaseApplications apps(store);
	REQUIRE(apps.AddApplicationManifest(PathToUtf8(Data("echo.vrmanifest")).c_str()) == VRApplicationError_None);
	uint32_t dead = ExitedProcessId();
	store.Update(StoreDoc::Applications, [&](Json::Value& doc) {
		doc["pids"][std::to_string(dead)] = "oc.test.echo";
		Json::Value& m = doc["manifests"]["template:oc.gone"];
		m["temporary"] = true;
		m["owner_pid"] = dead;
		m["apps"] = Json::Value(Json::arrayValue);
	});

	// Any change to the document
	CHECK(apps.SetDefaultApplicationForMimeType("oc.test.echo", "vr/oc-change") == VRApplicationError_None);
	Json::Value doc = store.Read(StoreDoc::Applications);
	CHECK_FALSE(doc["pids"].isMember(std::to_string(dead)));
	CHECK_FALSE(doc["manifests"].isMember("template:oc.gone"));
	CHECK(apps.IsApplicationInstalled("oc.test.echo"));

	// Values of the wrong type, as a broken file may have: no temporary manifest, no process id
	store.Update(StoreDoc::Applications, [&](Json::Value& d) {
		for (const std::string& path : d["manifests"].getMemberNames()) {
			d["manifests"][path]["temporary"] = "yes";
			d["manifests"][path]["owner_pid"] = -1;
		}
	});
	CHECK(apps.IsApplicationInstalled("oc.test.echo"));
	CHECK(apps.SetDefaultApplicationForMimeType("oc.test.echo", "vr/oc-change") == VRApplicationError_None);
}

TEST_CASE("Manifest_whose_file_is_gone_is_replaced")
{
	TempDir dir;
	SharedStore store(dir.path / "store");
	BaseApplications apps(store);
	fs::create_directories(dir.path / "old");
	fs::create_directories(dir.path / "new");
	fs::path old = dir.path / "old" / "echo.vrmanifest", moved = dir.path / "new" / "echo.vrmanifest";
	fs::copy_file(Data("echo.vrmanifest"), old);
	fs::copy_file(Data("echo.vrmanifest"), moved);
	CHECK(apps.AddApplicationManifest(PathToUtf8(old).c_str()) == VRApplicationError_None);
	CHECK(apps.AddApplicationManifest(PathToUtf8(moved).c_str()) == VRApplicationError_AppKeyAlreadyExists);

	// The game moved
	fs::remove(old);
	CHECK(apps.AddApplicationManifest(PathToUtf8(moved).c_str()) == VRApplicationError_None);
	CHECK(apps.GetApplicationCount() == 1);
	char binary[512];
	BaseApplications::EVRApplicationError error;
	apps.GetApplicationPropertyString("oc.test.echo", VRApplicationProperty_BinaryPath_String, binary, sizeof(binary), &error);
	CHECK(PathFromUtf8(binary).parent_path() == (dir.path / "new").lexically_normal());
}

TEST_CASE("Manifest_by_another_name_is_the_same_manifest")
{
	TempDir dir;
	SharedStore store(dir.path / "store");
	BaseApplications apps(store);
	fs::path first = dir.path / "echo.vrmanifest", second = dir.path / "echo_link.vrmanifest";
	fs::copy_file(Data("echo.vrmanifest"), first);
	// Another path to the same file, as a different case or an 8.3 name is on Windows
	fs::create_hard_link(first, second);
	CHECK(apps.AddApplicationManifest(PathToUtf8(first).c_str()) == VRApplicationError_None);
	CHECK(apps.AddApplicationManifest(PathToUtf8(second).c_str()) == VRApplicationError_None);
	CHECK(apps.GetApplicationCount() == 1);
}

TEST_CASE("Scene_process_is_this_one_when_it_is_a_scene_app")
{
	TempDir dir;
	SharedStore store(dir.path);
	BaseApplications apps(store);
#ifdef _WIN32
	uint32_t other = 4; // System, always there
#else
	uint32_t other = (uint32_t)getppid();
#endif
	store.Update(StoreDoc::Scene, [&](Json::Value& scene) {
		scene["pid"] = other;
		scene["app_key"] = "oc.other";
	});
	vr::EVRApplicationType saved = BaseClientCore::appType;
	BaseClientCore::appType = vr::VRApplication_Utility;
	CHECK(apps.GetCurrentSceneProcessId() == other);
	BaseClientCore::appType = vr::VRApplication_Scene;
	CHECK(apps.GetCurrentSceneProcessId() == oc_current_pid());
	BaseClientCore::appType = saved;
}

namespace {
std::vector<vr::VREvent_t> postedEvents;
void RecordEvent(const vr::VREvent_t& event)
{
	postedEvents.push_back(event);
}

size_t CountEvents(vr::EVREventType type)
{
	return (size_t)std::count_if(postedEvents.begin(), postedEvents.end(), [&](const vr::VREvent_t& e) { return e.eventType == type; });
}

// Records this program's events while it lives
struct EventRecorder {
	EventRecorder()
	{
		postedEvents.clear();
		OcSetLocalEventSink(&RecordEvent);
	}
	~EventRecorder() { OcSetLocalEventSink(nullptr); }
};
}

TEST_CASE("Application_list_changes_are_announced")
{
	TempDir dir;
	SharedStore store(dir.path);
	BaseApplications apps(store);
	EventRecorder recorder;

	CHECK(apps.AddApplicationManifest(PathToUtf8(Data("echo.vrmanifest")).c_str()) == VRApplicationError_None);
	CHECK(CountEvents(vr::VREvent_ApplicationListUpdated) == 1);
	// Nothing changed: nothing to announce
	CHECK(apps.AddApplicationManifest(PathToUtf8(Data("steamvr.vrsettings")).c_str()) == VRApplicationError_InvalidManifest);
	CHECK(apps.RemoveApplicationManifest(PathToUtf8(Data("overlay_apps.vrmanifest")).c_str()) == VRApplicationError_None);
	CHECK(CountEvents(vr::VREvent_ApplicationListUpdated) == 1);
	CHECK(apps.RemoveApplicationManifest(PathToUtf8(Data("echo.vrmanifest")).c_str()) == VRApplicationError_None);
	CHECK(CountEvents(vr::VREvent_ApplicationListUpdated) == 2);

	// A template launch adds an application
	fs::path manifest = dir.path / "template.vrmanifest";
	std::ofstream(manifest) << R"({"applications":[{"app_key":"oc.test.template","launch_type":"binary",)"
	                           R"("binary_path":"/nonexistent/oc_test_binary","is_template":true}]})";
	CHECK(apps.AddApplicationManifest(PathToUtf8(manifest).c_str()) == VRApplicationError_None);
	size_t before = CountEvents(vr::VREvent_ApplicationListUpdated);
	apps.LaunchTemplateApplication("oc.test.template", "oc.test.instance", nullptr, 0);
	CHECK(apps.IsApplicationInstalled("oc.test.instance"));
	CHECK(CountEvents(vr::VREvent_ApplicationListUpdated) == before + 1);
}

TEST_CASE("Scene_process_changes_are_announced")
{
	TempDir dir;
	SharedStore store(dir.path);
	EventRecorder recorder;

	BaseApplications::SetSceneProcess(store, true);
	CHECK(CountEvents(vr::VREvent_ProcessConnected) == 1);
	CHECK(CountEvents(vr::VREvent_SceneApplicationChanged) == 1);
	CHECK(CountEvents(vr::VREvent_SceneApplicationStateChanged) == 1);
	for (const vr::VREvent_t& e : postedEvents)
		if (e.eventType == vr::VREvent_ProcessConnected || e.eventType == vr::VREvent_SceneApplicationChanged)
			CHECK(e.data.process.pid == oc_current_pid());

	// Already the scene application: nothing changed
	BaseApplications::SetSceneProcess(store, true);
	CHECK(CountEvents(vr::VREvent_ProcessConnected) == 1);

	BaseApplications::SetSceneProcess(store, false);
	CHECK(CountEvents(vr::VREvent_ProcessDisconnected) == 1);
	CHECK(CountEvents(vr::VREvent_SceneApplicationChanged) == 2);
	CHECK(postedEvents.back().eventType == vr::VREvent_SceneApplicationStateChanged);
	const vr::VREvent_t& changed = postedEvents[postedEvents.size() - 3];
	CHECK(changed.eventType == vr::VREvent_SceneApplicationChanged);
	CHECK(changed.data.process.pid == 0);
	CHECK(changed.data.process.oldPid == oc_current_pid());
}

TEST_CASE("Scene_application_waits_until_it_has_drawn")
{
	TempDir dir;
	SharedStore store(dir.path);
	EventRecorder recorder;
	BaseApplications apps(store);

	BaseApplications::SetSceneProcess(store, true);
	CHECK(apps.GetSceneApplicationState() == EVRSceneApplicationState_Waiting);
	size_t before = CountEvents(vr::VREvent_SceneApplicationStateChanged);

	// Its first frame: running, and announced once
	BaseApplications::SetSceneDrawn(store);
	CHECK(apps.GetSceneApplicationState() == EVRSceneApplicationState_Running);
	CHECK(store.Read(StoreDoc::Scene)["drawn"].asBool());
	CHECK(CountEvents(vr::VREvent_SceneApplicationStateChanged) == before + 1);
	BaseApplications::SetSceneDrawn(store);
	CHECK(CountEvents(vr::VREvent_SceneApplicationStateChanged) == before + 1);

	// A new session of the same program keeps it
	BaseApplications::SetSceneProcess(store, true);
	CHECK(store.Read(StoreDoc::Scene)["drawn"].asBool());
	CHECK(apps.GetSceneApplicationState() == EVRSceneApplicationState_Running);

	// No longer the scene application, then the scene application again: waiting for its next frame
	BaseApplications::SetSceneProcess(store, false);
	BaseApplications::SetSceneProcess(store, true);
	CHECK(apps.GetSceneApplicationState() == EVRSceneApplicationState_Waiting);
	CHECK_FALSE(store.Read(StoreDoc::Scene)["drawn"].asBool());
	BaseApplications::SetSceneProcess(store, false);
}

TEST_CASE("Capsule_image_and_hidden_flag_are_properties")
{
	TempDir dir;
	SharedStore store(dir.path);
	BaseApplications apps(store);
	BaseApplications::EVRApplicationError error;

	fs::path manifest = dir.path / "hidden.vrmanifest";
	std::ofstream(manifest) << R"({"applications":[{"app_key":"oc.test.hidden","launch_type":"url","url":"https://example.invalid/",)"
	                           R"("image_path_capsule":"capsule.png","is_hidden":true},)"
	                           R"({"app_key":"oc.test.shown","launch_type":"url","url":"https://example.invalid/"}]})";
	REQUIRE(apps.AddApplicationManifest(PathToUtf8(manifest).c_str()) == VRApplicationError_None);

	char value[512];
	CHECK(apps.GetApplicationPropertyString("oc.test.hidden", VRApplicationProperty_ImagePathCapsule_String, value, sizeof(value), &error) > 1);
	CHECK(error == VRApplicationError_None);
	CHECK(fs::path(value) == dir.path / "capsule.png");
	apps.GetApplicationPropertyString("oc.test.shown", VRApplicationProperty_ImagePathCapsule_String, value, sizeof(value), &error);
	CHECK(error == VRApplicationError_PropertyNotSet);

	CHECK(apps.GetApplicationPropertyBool("oc.test.hidden", VRApplicationProperty_IsHidden_Bool, &error));
	CHECK(error == VRApplicationError_None);
	CHECK_FALSE(apps.GetApplicationPropertyBool("oc.test.shown", VRApplicationProperty_IsHidden_Bool, &error));
	CHECK(error == VRApplicationError_None);
}

TEST_CASE("Process_id_taken_over_by_another_program_is_not_the_application")
{
	TempDir dir;
	SharedStore store(dir.path);
	BaseApplications apps(store);
	REQUIRE(apps.AddApplicationManifest(PathToUtf8(Data("echo.vrmanifest")).c_str()) == VRApplicationError_None);
#ifdef _WIN32
	uint32_t other = 4; // System, always there
#else
	uint32_t other = (uint32_t)getppid();
#endif
	uint64_t start = SharedStore::ProcessStartTime(other);
	REQUIRE(start != 0);
	char key[64];

	// Listed by its start time: known
	REQUIRE(apps.RegisterSubprocess(other) == VRApplicationError_None);
	CHECK(apps.GetApplicationKeyByProcessId(other, key, sizeof(key)) == VRApplicationError_None);
	CHECK(apps.GetApplicationProcessId(CurrentProcessAppKey(store).c_str()) != 0);

	// The id listed for a process that started at another time
	store.Update(StoreDoc::Applications, [&](Json::Value& doc) {
		doc["pids"][std::to_string(other)] = "oc.test.echo";
		doc["pid_starts"][std::to_string(other)] = (Json::UInt64)(start + 1);
	});
	CHECK(apps.GetApplicationKeyByProcessId(other, key, sizeof(key)) == VRApplicationError_UnknownApplication);
	CHECK(apps.GetApplicationProcessId("oc.test.echo") == 0);

	// Dropped at the next change to the document
	CHECK(apps.SetDefaultApplicationForMimeType("oc.test.echo", "vr/oc-change") == VRApplicationError_None);
	Json::Value doc = store.Read(StoreDoc::Applications);
	CHECK_FALSE(doc["pids"].isMember(std::to_string(other)));
	CHECK_FALSE(doc["pid_starts"].isMember(std::to_string(other)));
}

TEST_CASE("Template_instance_belongs_to_the_program_started_for_it")
{
	TempDir dir;
	SharedStore store(dir.path);
	BaseApplications apps(store);

	fs::path manifest = dir.path / "template.vrmanifest";
	Json::Value app(Json::objectValue);
	app["app_key"] = "oc.test.template";
	app["launch_type"] = "binary";
	// Runs for a while, so the instance is still there to look at
	app["binary_path"] = OC_ECHO_APP;
	app["arguments"] = "marker.txt --stay 2";
	app["working_directory"] = PathToUtf8(dir.path);
	app["is_template"] = true;
	Json::Value root(Json::objectValue);
	root["applications"].append(app);
	std::ofstream(manifest) << root.toStyledString();
	REQUIRE(apps.AddApplicationManifest(PathToUtf8(manifest).c_str()) == VRApplicationError_None);

	REQUIRE(apps.LaunchTemplateApplication("oc.test.template", "oc.test.instance", nullptr, 0) == VRApplicationError_None);
	Json::Value doc = store.Read(StoreDoc::Applications);
	Json::Value instance = doc["manifests"]["template:oc.test.instance"];
	REQUIRE(instance.isObject());
	uint32_t owner = instance["owner_pid"].asUInt();
	CHECK(owner != oc_current_pid());
	CHECK(doc["pids"][std::to_string(owner)].asString() == "oc.test.instance");

	// It ends with that program
	bool ended = false;
	for (int i = 0; i < 100 && !ended; i++) {
		ended = !SharedStore::IsProcessAlive(owner);
		if (!ended)
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	CHECK(ended);
	CHECK_FALSE(apps.IsApplicationInstalled("oc.test.instance"));
}

TEST_CASE("Launching_a_running_application_is_refused")
{
	TempDir dir;
	SharedStore store(dir.path);
	BaseApplications apps(store);
	REQUIRE(apps.AddApplicationManifest(PathToUtf8(Data("echo.vrmanifest")).c_str()) == VRApplicationError_None);
#ifdef _WIN32
	uint32_t other = 4; // System, always there
#else
	uint32_t other = (uint32_t)getppid();
#endif
	CHECK(apps.GetApplicationProcessId("oc.test.echo") == 0);
	store.Update(StoreDoc::Applications, [&](Json::Value& doc) { doc["pids"][std::to_string(other)] = "oc.test.echo"; });
	REQUIRE(apps.GetApplicationProcessId("oc.test.echo") == other);
	CHECK(apps.LaunchApplication("oc.test.echo") == VRApplicationError_ApplicationAlreadyRunning);
}

TEST_CASE("Autolaunch_starts_only_flagged_dashboard_overlays")
{
	TempDir dir;
	SharedStore store(dir.path);
	BaseApplications apps(store);
	apps.AddApplicationManifest(PathToUtf8(Data("echo.vrmanifest")).c_str());
	apps.AddApplicationManifest(PathToUtf8(Data("overlay_apps.vrmanifest")).c_str());
	CHECK(StartAutolaunchOverlays(apps).empty());

	// A program that isn't a dashboard overlay takes no flag, and one set by hand starts nothing
	CHECK(apps.SetApplicationAutoLaunch("oc.test.echo", true) == VRApplicationError_InvalidApplication);
	CHECK_FALSE(apps.GetApplicationAutoLaunch("oc.test.echo"));
	store.Update(StoreDoc::Applications, [](Json::Value& doc) { doc["autolaunch"]["oc.test.echo"] = true; });
	CHECK(StartAutolaunchOverlays(apps).empty());

	CHECK(apps.SetApplicationAutoLaunch("oc.test.overlay", true) == VRApplicationError_None);
	std::vector<std::string> tried = StartAutolaunchOverlays(apps);
	REQUIRE(tried.size() == 1);
	CHECK(tried[0] == "oc.test.overlay");

	CHECK(apps.SetApplicationAutoLaunch("oc.test.overlay", false) == VRApplicationError_None);
	CHECK(StartAutolaunchOverlays(apps).empty());
}

namespace {
// A manifest for one scene application that writes marker.txt in folder when it runs
fs::path WriteEchoManifest(const fs::path& folder, const char* key)
{
	fs::path manifest = folder / (std::string(key) + ".vrmanifest");
	Json::Value app(Json::objectValue);
	app["app_key"] = key;
	app["launch_type"] = "binary";
	app["binary_path"] = OC_ECHO_APP;
	app["arguments"] = "marker.txt started";
	app["working_directory"] = PathToUtf8(folder);
	Json::Value root(Json::objectValue);
	root["applications"].append(app);
	std::ofstream(manifest) << root.toStyledString();
	return manifest;
}

// The events another program posted to the store for the program with process id pid
std::vector<vr::VREvent_t> EventsFor(SharedStore& reader, uint64_t& cursor, uint32_t pid)
{
	std::vector<vr::VREvent_t> out;
	for (const StoreEvent& s : reader.PollEvents(cursor))
		if (std::optional<vr::VREvent_t> e = OcEventForProgram(s, pid))
			out.push_back(*e);
	return out;
}

Json::UInt64 NowMs()
{
	return (Json::UInt64)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

struct AppType {
	vr::EVRApplicationType saved = BaseClientCore::appType;
	explicit AppType(vr::EVRApplicationType type) { BaseClientCore::appType = type; }
	~AppType() { BaseClientCore::appType = saved; }
};
} // namespace

TEST_CASE("Event_for_one_program_reaches_only_it")
{
	TempDir dir;
	SharedStore a(dir.path), b(dir.path);
	uint64_t cursor = 0;
	b.PollEvents(cursor);
	vr::VREvent_Data_t data{};
	data.process.pid = 1234;
	data.process.oldPid = 99;
	REQUIRE(OcPostEventTo(a, 1234, vr::VREvent_Quit, 5, data));
	OcPostEvent(vr::VREvent_ApplicationListUpdated, 0, vr::VREvent_Data_t{}, false); // never in the store

	std::vector<StoreEvent> got = b.PollEvents(cursor);
	REQUIRE(got.size() == 1);
	CHECK(OcIsPrivateEvent(got[0].eventType)); // older programs leave it alone
	CHECK_FALSE(OcEventForProgram(got[0], 4321));
	std::optional<vr::VREvent_t> e = OcEventForProgram(got[0], 1234);
	REQUIRE(e);
	CHECK(e->eventType == vr::VREvent_Quit);
	CHECK(e->trackedDeviceIndex == 5);
	CHECK(e->data.process.pid == 1234);
	CHECK(e->data.process.oldPid == 99);

	// A private type addressed to it is still OpenLamina's own
	StoreEvent own = got[0];
	own.data[2] = (uint64_t)OcEvent_RemotePointer | ((uint64_t)1234 << 32);
	CHECK_FALSE(OcEventForProgram(own, 1234));

	// A public event is every program's
	StoreEvent pub{};
	pub.eventType = vr::VREvent_ApplicationListUpdated;
	CHECK(OcEventForProgram(pub, 4321));

	// This program's own: its own queue, never the store
	EventRecorder recorder;
	REQUIRE(OcPostEventTo(a, oc_current_pid(), vr::VREvent_Quit, 0, data));
	CHECK(CountEvents(vr::VREvent_Quit) == 1);
	CHECK(b.PollEvents(cursor).empty());
}

TEST_CASE("Prelaunch_check_asks_only_the_scene_application_to_quit")
{
	TempDir dir;
	SharedStore store(dir.path), other(dir.path);
	BaseApplications apps(store);
	AppType type(vr::VRApplication_Utility);
	EventRecorder recorder;
	REQUIRE(apps.AddApplicationManifest(PathToUtf8(WriteEchoManifest(dir.path, "oc.test.next")).c_str()) == VRApplicationError_None);
	REQUIRE(apps.AddApplicationManifest(PathToUtf8(WriteEchoManifest(dir.path, "oc.test.third")).c_str()) == VRApplicationError_None);
	uint64_t cursor = 0;
	other.PollEvents(cursor);

	// No scene application: nothing to wait for
	CHECK(apps.PerformApplicationPrelaunchCheck("oc.test.next") == VRApplicationError_None);
	CHECK(apps.GetTransitionState() == VRApplicationTransition_None);

#ifdef _WIN32
	uint32_t scene = 4; // System, always there
#else
	uint32_t scene = (uint32_t)getppid();
#endif
	store.Update(StoreDoc::Scene, [&](Json::Value& doc) {
		doc["pid"] = scene;
		doc["start"] = (Json::UInt64)SharedStore::ProcessStartTime(scene);
		doc["app_key"] = "oc.other";
	});
	CHECK(apps.PerformApplicationPrelaunchCheck("oc.test.next") == VRApplicationError_OldApplicationQuitting);
	std::vector<vr::VREvent_t> sent = EventsFor(other, cursor, scene);
	REQUIRE(sent.size() == 1);
	CHECK(sent[0].eventType == vr::VREvent_Quit);
	CHECK(sent[0].data.process.pid == scene);
	CHECK(EventsFor(other, cursor, scene + 1).empty()); // nobody else's
	CHECK(CountEvents(vr::VREvent_Quit) == 0); // not this program's either
	CHECK(CountEvents((vr::EVREventType)1300) == 1); // the transition started

	CHECK(apps.GetTransitionState() == VRApplicationTransition_OldAppQuitSent);
	char key[64];
	CHECK(apps.GetStartingApplication(key, sizeof(key)) == VRApplicationError_None);
	CHECK(std::string(key) == "oc.test.next");

	// Asked again while it quits: no second request; another application waits its turn
	CHECK(apps.PerformApplicationPrelaunchCheck("oc.test.next") == VRApplicationError_OldApplicationQuitting);
	CHECK(EventsFor(other, cursor, scene).empty());
	CHECK(apps.PerformApplicationPrelaunchCheck("oc.test.third") == VRApplicationError_LaunchInProgress);

	// Stopped: over, and another application may start
	apps.AbortTransition();
	CHECK(CountEvents((vr::EVREventType)1301) == 1);
	CHECK(apps.GetTransitionState() == VRApplicationTransition_None);
	CHECK(apps.PerformApplicationPrelaunchCheck("oc.test.third") == VRApplicationError_OldApplicationQuitting);

	// Once it has gone, the transition is over
	store.Update(StoreDoc::Scene, [&](Json::Value& doc) { doc["transition"]["old_pid"] = ExitedProcessId(); });
	CHECK(apps.GetTransitionState() == VRApplicationTransition_None);
	CHECK(apps.GetStartingApplication(key, sizeof(key)) == VRApplicationError_NoApplication);
}

TEST_CASE("Launch_waits_for_the_scene_application_to_quit")
{
	TempDir dir;
	SharedStore store(dir.path);
	BaseApplications apps(store);
	AppType type(vr::VRApplication_Scene);
	EventRecorder recorder;
	REQUIRE(apps.AddApplicationManifest(PathToUtf8(WriteEchoManifest(dir.path, "oc.test.next")).c_str()) == VRApplicationError_None);

	// Launched with no scene application: nobody to wait for; it's the starting application (as SteamVR answers)
	// until its session starts
	fs::create_directories(dir.path / "first");
	REQUIRE(apps.AddApplicationManifest(PathToUtf8(WriteEchoManifest(dir.path / "first", "oc.test.first")).c_str()) == VRApplicationError_None);
	CHECK(apps.LaunchApplication("oc.test.first") == VRApplicationError_None);
	CHECK(CountEvents(vr::VREvent_Quit) == 0);
	CHECK(apps.GetTransitionState() == VRApplicationTransition_NewAppLaunched);
	char starting[64] = {};
	CHECK(apps.GetStartingApplication(starting, sizeof(starting)) == VRApplicationError_None);
	CHECK(std::string(starting) == "oc.test.first");

	// This program becomes the scene application instead (the first one never starts a session): that launch is
	// over, and this one launches another
	BaseApplications::SetSceneProcess(store, true);
	CHECK(apps.GetTransitionState() == VRApplicationTransition_None);
	postedEvents.clear();
	CHECK(apps.LaunchApplication("oc.test.next") == VRApplicationError_None);
	REQUIRE(CountEvents(vr::VREvent_Quit) == 1);
	CHECK(postedEvents.front().data.process.pid == oc_current_pid());
	CHECK(apps.GetTransitionState() == VRApplicationTransition_OldAppQuitSent);
	CHECK_FALSE(fs::exists(dir.path / "marker.txt")); // not before this one has gone

	// It quits: the new one starts
	BaseApplications::SetSceneProcess(store, false);
	CHECK(CountEvents(vr::VREvent_ProcessQuit) == 1);
	CHECK(CountEvents((vr::EVREventType)1302) == 1);
	CHECK(apps.GetTransitionState() == VRApplicationTransition_NewAppLaunched);
	bool started = false;
	for (int i = 0; i < 50 && !started; i++) {
		started = fs::exists(dir.path / "marker.txt");
		if (!started)
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	CHECK(started);

	// And the transition ends when it becomes the scene application
	REQUIRE(apps.IdentifyApplication(0, "oc.test.next") == VRApplicationError_None);
	BaseApplications::SetSceneProcess(store, true);
	CHECK(CountEvents((vr::EVREventType)1305) == 1);
	CHECK(apps.GetTransitionState() == VRApplicationTransition_None);
	BaseApplications::SetSceneProcess(store, false);
}

TEST_CASE("Launch_with_none_running_starts_until_its_session")
{
	TempDir dir;
	SharedStore store(dir.path);
	BaseApplications apps(store);
	AppType type(vr::VRApplication_Scene);
	EventRecorder recorder;
	REQUIRE(apps.AddApplicationManifest(PathToUtf8(WriteEchoManifest(dir.path, "oc.test.first")).c_str()) == VRApplicationError_None);
	fs::create_directories(dir.path / "other");
	REQUIRE(apps.AddApplicationManifest(PathToUtf8(WriteEchoManifest(dir.path / "other", "oc.test.other")).c_str()) == VRApplicationError_None);

	CHECK(apps.LaunchApplication("oc.test.first") == VRApplicationError_None);
	CHECK(apps.GetTransitionState() == VRApplicationTransition_NewAppLaunched);
	CHECK(CountEvents(vr::VREvent_SceneApplicationStateChanged) == 1);
	CHECK(CountEvents((vr::EVREventType)1302) == 1); // the new application started
	// Another launch waits for it
	CHECK(apps.LaunchApplication("oc.test.other") == VRApplicationError_LaunchInProgress);

	// Its session: no longer starting
	REQUIRE(apps.IdentifyApplication(0, "oc.test.first") == VRApplicationError_None);
	BaseApplications::SetSceneProcess(store, true);
	CHECK(CountEvents((vr::EVREventType)1305) == 1);
	CHECK(apps.GetTransitionState() == VRApplicationTransition_None);
	BaseApplications::SetSceneProcess(store, false);
}

TEST_CASE("Launch_with_none_running_that_fails_isnt_starting")
{
	TempDir dir;
	SharedStore store(dir.path);
	BaseApplications apps(store);
	AppType type(vr::VRApplication_Scene);
	EventRecorder recorder;
	Json::Value app(Json::objectValue);
	app["app_key"] = "oc.test.missing";
	app["launch_type"] = "binary";
	app["binary_path"] = PathToUtf8(dir.path / "does_not_exist"); // no .exe: refused before the launch on Linux
	Json::Value root(Json::objectValue);
	root["applications"].append(app);
	const fs::path manifest = dir.path / "missing.vrmanifest";
	std::ofstream(manifest) << root.toStyledString();
	REQUIRE(apps.AddApplicationManifest(PathToUtf8(manifest).c_str()) == VRApplicationError_None);

	CHECK(apps.LaunchApplication("oc.test.missing") == VRApplicationError_LaunchFailed);
	CHECK(apps.GetTransitionState() == VRApplicationTransition_None);
	CHECK(CountEvents((vr::EVREventType)1301) == 1); // aborted
}

TEST_CASE("A_switch_keeps_its_launch_when_another_becomes_the_scene_application")
{
	TempDir dir;
	SharedStore store(dir.path);
	BaseApplications apps(store);
	AppType type(vr::VRApplication_Scene);
	// A switch's new application launched (the old one gone): kept while a third program's session starts
	store.Update(StoreDoc::Scene, [&](Json::Value& doc) {
		doc["transition"]["state"] = VRApplicationTransition_NewAppLaunched;
		doc["transition"]["app_key"] = "oc.test.next";
		doc["transition"]["old_pid"] = 12345;
		doc["transition"]["since_ms"] = NowMs();
	});
	BaseApplications::SetSceneProcess(store, true);
	CHECK(apps.GetTransitionState() == VRApplicationTransition_NewAppLaunched);
	BaseApplications::SetSceneProcess(store, false);
}

TEST_CASE("Mime_launch_of_a_running_application_hands_it_the_arguments")
{
	TempDir dir;
	SharedStore store(dir.path), other(dir.path);
	BaseApplications apps(store);
	EventRecorder recorder;
	REQUIRE(apps.AddApplicationManifest(PathToUtf8(Data("echo.vrmanifest")).c_str()) == VRApplicationError_None);

	// This program is the application: the event is its own
	REQUIRE(apps.IdentifyApplication(0, "oc.test.echo") == VRApplicationError_None);
	CHECK(apps.LaunchApplicationFromMimeType("vr/oc-echo", "file.oc --open") == VRApplicationError_None);
	REQUIRE(CountEvents(vr::VREvent_ApplicationMimeTypeLoad) == 1);
	const vr::VREvent_t load = postedEvents.back();
	CHECK(load.data.applicationLaunch.pid == oc_current_pid());
	char args[64];
	CHECK(apps.GetApplicationLaunchArguments(load.data.applicationLaunch.unArgsHandle, args, sizeof(args)) == strlen("file.oc --open") + 1);
	CHECK(std::string(args) == "file.oc --open");
	CHECK(apps.GetApplicationLaunchArguments(load.data.applicationLaunch.unArgsHandle + 1, args, sizeof(args)) == 0);
	CHECK(std::string(args).empty());

	// Another program's: to it alone, by the store, and only it reads the arguments
#ifdef _WIN32
	uint32_t running = 4; // System, always there
#else
	uint32_t running = (uint32_t)getppid();
#endif
	REQUIRE(apps.IdentifyApplication(0, "oc.self") == VRApplicationError_None);
	REQUIRE(apps.IdentifyApplication(running, "oc.test.echo") == VRApplicationError_None);
	uint64_t cursor = 0;
	other.PollEvents(cursor);
	CHECK(apps.LaunchApplicationFromMimeType("vr/oc-echo", "second") == VRApplicationError_None);
	std::vector<vr::VREvent_t> got = EventsFor(other, cursor, running);
	REQUIRE(got.size() == 1);
	CHECK(got[0].eventType == vr::VREvent_ApplicationMimeTypeLoad);
	CHECK(got[0].data.applicationLaunch.pid == running);
	CHECK(got[0].data.applicationLaunch.unArgsHandle != load.data.applicationLaunch.unArgsHandle);
	CHECK(apps.GetApplicationLaunchArguments(got[0].data.applicationLaunch.unArgsHandle, args, sizeof(args)) == 0);
	Json::Value entry = store.Read(StoreDoc::Applications)["launch_args"][std::to_string(got[0].data.applicationLaunch.unArgsHandle)];
	CHECK(entry["args"].asString() == "second");
}

TEST_CASE("Process_lists_itself_at_init")
{
	TempDir dir;
	SharedStore store(dir.path);
	BaseApplications apps(store);
	const std::string self = std::to_string(oc_current_pid());

	BaseApplications::AddThisProcess(store);
	Json::Value doc = store.Read(StoreDoc::Applications);
	CHECK(doc["pids"][self].asString() == CurrentProcessAppKey(store));
	char key[128];
	CHECK(apps.GetApplicationKeyByProcessId(oc_current_pid(), key, sizeof(key)) == VRApplicationError_None);

	{
		// Its own entry doesn't hide what it's known by later
		AppId id("4242");
		CHECK(CurrentProcessAppKey(store) == "steam.app.4242");
	}

	// IdentifyApplication's entry is kept at a later init
	REQUIRE(apps.IdentifyApplication(0, "oc.named") == VRApplicationError_None);
	BaseApplications::AddThisProcess(store);
	CHECK(store.Read(StoreDoc::Applications)["pids"][self].asString() == "oc.named");
	CHECK(CurrentProcessAppKey(store) == "oc.named");
}

TEST_CASE("Other_programs_see_whether_the_scene_application_has_a_dashboard")
{
	const ProcessAlive alive = [](uint32_t pid, uint64_t) { return pid == 10; };
	Json::Value scene(Json::objectValue);
	CHECK(SceneDocHasDashboard(scene, alive)); // none running
	scene["pid"] = 10;
	CHECK(SceneDocHasDashboard(scene, alive)); // one that doesn't say
	scene["dashboard"] = false;
	CHECK_FALSE(SceneDocHasDashboard(scene, alive));
	scene["pid"] = 11; // one that ended
	CHECK(SceneDocHasDashboard(scene, alive));

	// What a scene application's session writes
	TempDir dir;
	SharedStore store(dir.path);
	BaseApplications::SetSceneProcess(store, true);
	Json::Value written = store.Read(StoreDoc::Scene);
	REQUIRE(written["dashboard"].isBool());
	CHECK(written["dashboard"].asBool() == (DashboardButtonFromSetting(oovr_global_configuration.DashboardButton()) != Dashboard::Button::Off));
	BaseApplications::SetSceneProcess(store, false);
	CHECK(DashboardButtonFromSetting("menu_long") == Dashboard::Button::MenuLong);
	CHECK(DashboardButtonFromSetting("off") == Dashboard::Button::Off);
	CHECK(DashboardButtonFromSetting("") == Dashboard::Button::Off);
}

TEST_CASE("A_launch_under_way_holds_off_others_once_the_old_application_has_gone")
{
	TempDir dir;
	SharedStore store(dir.path);
	BaseApplications apps(store);
	AppType type(vr::VRApplication_Utility);
	REQUIRE(apps.AddApplicationManifest(PathToUtf8(WriteEchoManifest(dir.path, "oc.test.next")).c_str()) == VRApplicationError_None);
	REQUIRE(apps.AddApplicationManifest(PathToUtf8(WriteEchoManifest(dir.path, "oc.test.third")).c_str()) == VRApplicationError_None);
	// The old scene application has gone and the new one is starting: no scene application is named
	store.Update(StoreDoc::Scene, [&](Json::Value& doc) {
		doc["transition"]["state"] = VRApplicationTransition_NewAppLaunched;
		doc["transition"]["app_key"] = "oc.test.next";
		doc["transition"]["since_ms"] = NowMs();
	});
	CHECK(apps.PerformApplicationPrelaunchCheck("oc.test.next") == VRApplicationError_ApplicationAlreadyStarting);
	CHECK(apps.PerformApplicationPrelaunchCheck("oc.test.third") == VRApplicationError_LaunchInProgress);
	CHECK(apps.LaunchApplication("oc.test.third") == VRApplicationError_LaunchInProgress);
	CHECK_FALSE(fs::exists(dir.path / "marker.txt"));
}

TEST_CASE("A_launch_left_by_an_application_that_ended_without_its_session_s_end_is_taken_over")
{
	TempDir dir;
	SharedStore store(dir.path);
	BaseApplications apps(store);
	AppType type(vr::VRApplication_Utility);
	EventRecorder recorder;
	REQUIRE(apps.AddApplicationManifest(PathToUtf8(WriteEchoManifest(dir.path, "oc.test.next")).c_str()) == VRApplicationError_None);
	const uint32_t gone = ExitedProcessId();
	store.Update(StoreDoc::Scene, [&](Json::Value& doc) {
		doc["transition"]["state"] = VRApplicationTransition_OldAppQuitSent;
		doc["transition"]["app_key"] = "oc.test.next";
		doc["transition"]["old_pid"] = gone;
		doc["transition"]["launch"] = true;
		doc["transition"]["since_ms"] = NowMs();
	});
	// The next look at the transition launches it, once
	CHECK(apps.GetTransitionState() == VRApplicationTransition_NewAppLaunched);
	CHECK(apps.GetTransitionState() == VRApplicationTransition_NewAppLaunched);
	CHECK(CountEvents((vr::EVREventType)1302) == 1);
	bool started = false;
	for (int i = 0; i < 50 && !started; i++) {
		started = fs::exists(dir.path / "marker.txt");
		if (!started)
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	CHECK(started);
}

TEST_CASE("Listing_itself_again_writes_nothing")
{
	TempDir dir;
	SharedStore store(dir.path);
	BaseApplications::AddThisProcess(store);
	const uint64_t generation = store.Generation(StoreDoc::Applications);
	BaseApplications::AddThisProcess(store);
	CHECK(store.Generation(StoreDoc::Applications) == generation);
}

TEST_CASE("Asking_about_a_transition_whose_old_application_runs_writes_nothing")
{
	TempDir dir;
	SharedStore store(dir.path);
	BaseApplications apps(store);
#ifdef _WIN32
	uint32_t scene = 4; // System, always there
#else
	uint32_t scene = (uint32_t)getppid();
#endif
	store.Update(StoreDoc::Scene, [&](Json::Value& doc) {
		doc["transition"]["state"] = VRApplicationTransition_OldAppQuitSent;
		doc["transition"]["app_key"] = "oc.test.next";
		doc["transition"]["old_pid"] = scene;
		doc["transition"]["old_start"] = (Json::UInt64)SharedStore::ProcessStartTime(scene);
		doc["transition"]["launch"] = true;
		doc["transition"]["since_ms"] = NowMs();
	});
	const uint64_t generation = store.Generation(StoreDoc::Scene);
	CHECK(apps.GetTransitionState() == VRApplicationTransition_OldAppQuitSent);
	CHECK(store.Generation(StoreDoc::Scene) == generation);
}

TEST_CASE("Scene_application_state_follows_a_transition")
{
	TempDir dir;
	SharedStore store(dir.path);
	BaseApplications apps(store);
	AppType type(vr::VRApplication_Utility);
#ifdef _WIN32
	uint32_t scene = 4; // System, always there
#else
	uint32_t scene = (uint32_t)getppid();
#endif
	// Another program is the scene application, has drawn, and is asked to quit for the next
	store.Update(StoreDoc::Scene, [&](Json::Value& doc) {
		doc["pid"] = scene;
		doc["start"] = (Json::UInt64)SharedStore::ProcessStartTime(scene);
		doc["drawn"] = true;
	});
	CHECK(apps.GetSceneApplicationState() == EVRSceneApplicationState_Running);
	store.Update(StoreDoc::Scene, [&](Json::Value& doc) {
		doc["transition"]["state"] = VRApplicationTransition_OldAppQuitSent;
		doc["transition"]["app_key"] = "oc.test.next";
		doc["transition"]["old_pid"] = scene;
		doc["transition"]["old_start"] = (Json::UInt64)SharedStore::ProcessStartTime(scene);
		doc["transition"]["since_ms"] = NowMs();
	});
	CHECK(apps.GetSceneApplicationState() == EVRSceneApplicationState_Quitting);

	// It has gone and the next is launched: starting, until its session starts
	store.Update(StoreDoc::Scene, [&](Json::Value& doc) {
		doc = Json::Value(Json::objectValue);
		doc["transition"]["state"] = VRApplicationTransition_NewAppLaunched;
		doc["transition"]["app_key"] = "oc.test.next";
		doc["transition"]["since_ms"] = NowMs();
	});
	CHECK(apps.GetSceneApplicationState() == EVRSceneApplicationState_Starting);

	// Aborted: no scene application, and the state change is announced
	EventRecorder recorder;
	apps.AbortTransition();
	CHECK(apps.GetSceneApplicationState() == EVRSceneApplicationState_None);
	CHECK(CountEvents(vr::VREvent_SceneApplicationStateChanged) == 1);
}

TEST_CASE("A_launch_asked_for_long_ago_is_dropped_not_taken_over")
{
	TempDir dir;
	SharedStore store(dir.path);
	BaseApplications apps(store);
	AppType type(vr::VRApplication_Utility);
	REQUIRE(apps.AddApplicationManifest(PathToUtf8(WriteEchoManifest(dir.path, "oc.test.next")).c_str()) == VRApplicationError_None);
	const uint32_t gone = ExitedProcessId();
	store.Update(StoreDoc::Scene, [&](Json::Value& doc) {
		doc["transition"]["state"] = VRApplicationTransition_OldAppQuitSent;
		doc["transition"]["app_key"] = "oc.test.next";
		doc["transition"]["old_pid"] = gone;
		doc["transition"]["launch"] = true;
		doc["transition"]["since_ms"] = (Json::UInt64)1; // long ago
	});
	EventRecorder recorder;
	CHECK(apps.GetTransitionState() == VRApplicationTransition_None);
	CHECK_FALSE(store.Read(StoreDoc::Scene).isMember("transition"));
	CHECK(CountEvents((vr::EVREventType)1301) == 1);
	CHECK(CountEvents((vr::EVREventType)1302) == 0); // nothing launched
}

TEST_CASE("A_taken_over_launch_starts_no_second_copy")
{
	TempDir dir;
	SharedStore store(dir.path);
	BaseApplications apps(store);
	AppType type(vr::VRApplication_Utility);
	REQUIRE(apps.AddApplicationManifest(PathToUtf8(WriteEchoManifest(dir.path, "oc.test.next")).c_str()) == VRApplicationError_None);
	// Started by hand meanwhile: this program is it
	REQUIRE(apps.IdentifyApplication(0, "oc.test.next") == VRApplicationError_None);
	const uint32_t gone = ExitedProcessId();
	store.Update(StoreDoc::Scene, [&](Json::Value& doc) {
		doc["transition"]["state"] = VRApplicationTransition_OldAppQuitSent;
		doc["transition"]["app_key"] = "oc.test.next";
		doc["transition"]["old_pid"] = gone;
		doc["transition"]["launch"] = true;
		doc["transition"]["since_ms"] = NowMs();
	});
	// Left for its own session's start to end
	EventRecorder recorder;
	CHECK(apps.GetTransitionState() == VRApplicationTransition_NewAppLaunched);
	CHECK(CountEvents((vr::EVREventType)1302) == 0); // nothing launched
}

TEST_CASE("A_quit_request_holds_other_launches_off_for_a_time_only")
{
	TempDir dir;
	SharedStore store(dir.path), other(dir.path);
	BaseApplications apps(store);
	AppType type(vr::VRApplication_Utility);
	REQUIRE(apps.AddApplicationManifest(PathToUtf8(WriteEchoManifest(dir.path, "oc.test.next")).c_str()) == VRApplicationError_None);
	REQUIRE(apps.AddApplicationManifest(PathToUtf8(WriteEchoManifest(dir.path, "oc.test.third")).c_str()) == VRApplicationError_None);
#ifdef _WIN32
	uint32_t scene = 4; // System, always there
#else
	uint32_t scene = (uint32_t)getppid();
#endif
	store.Update(StoreDoc::Scene, [&](Json::Value& doc) {
		doc["pid"] = scene;
		doc["start"] = (Json::UInt64)SharedStore::ProcessStartTime(scene);
		doc["app_key"] = "oc.other";
	});
	uint64_t cursor = 0;
	other.PollEvents(cursor);
	CHECK(apps.PerformApplicationPrelaunchCheck("oc.test.next") == VRApplicationError_OldApplicationQuitting);
	const uint64_t first = store.Read(StoreDoc::Scene)["transition"]["since_ms"].asUInt64();
	// Asked again: the first ask's time is kept
	CHECK(apps.PerformApplicationPrelaunchCheck("oc.test.next") == VRApplicationError_OldApplicationQuitting);
	CHECK(store.Read(StoreDoc::Scene)["transition"]["since_ms"].asUInt64() == first);
	CHECK(EventsFor(other, cursor, scene).size() == 1);

	// The old application never quit: the request lapses, and the next ask sends it again
	store.Update(StoreDoc::Scene, [&](Json::Value& doc) { doc["transition"]["since_ms"] = (Json::UInt64)1; });
	CHECK(apps.GetTransitionState() == VRApplicationTransition_None);
	CHECK(apps.PerformApplicationPrelaunchCheck("oc.test.third") == VRApplicationError_OldApplicationQuitting);
	CHECK(EventsFor(other, cursor, scene).size() == 1);
}

TEST_CASE("A_quit_request_that_cannot_be_sent_is_undone")
{
	TempDir dir;
	SharedStore store(dir.path);
	BaseApplications apps(store);
	AppType type(vr::VRApplication_Scene);
	REQUIRE(apps.AddApplicationManifest(PathToUtf8(WriteEchoManifest(dir.path, "oc.test.next")).c_str()) == VRApplicationError_None);
	// This program is the scene application, and has no event queue to take the quit
	BaseApplications::SetSceneProcess(store, true);
	OcSetLocalEventSink(nullptr);
	CHECK(apps.PerformApplicationPrelaunchCheck("oc.test.next") == VRApplicationError_IPCFailed);
	CHECK_FALSE(store.Read(StoreDoc::Scene).isMember("transition"));
	CHECK(apps.GetTransitionState() == VRApplicationTransition_None);
	BaseApplications::SetSceneProcess(store, false);
}

TEST_CASE("Overlay_command_crosses_between_programs")
{
	OcOverlayCommand flag;
	flag.pid = 4321;
	flag.handle = 0x1234567890ull;
	flag.kind = OcOverlayCommand::Flag;
	flag.flag = 15;
	flag.enabled = true;
	std::optional<std::array<uint64_t, 4>> packed = OcPackOverlayCommand(flag);
	REQUIRE(packed);
	uint64_t raw[4] = { (*packed)[0], (*packed)[1], (*packed)[2], (*packed)[3] };
	OcOverlayCommand back = OcUnpackOverlayCommand(raw);
	CHECK(back.pid == 4321);
	CHECK(back.handle == 0x1234567890ull);
	CHECK(back.kind == OcOverlayCommand::Flag);
	CHECK(back.flag == 15);
	CHECK(back.enabled);

	OcOverlayCommand alpha;
	alpha.kind = OcOverlayCommand::Alpha;
	alpha.number = 0.25f;
	packed = OcPackOverlayCommand(alpha);
	REQUIRE(packed);
	uint64_t rawAlpha[4] = { (*packed)[0], (*packed)[1], (*packed)[2], (*packed)[3] };
	CHECK(OcUnpackOverlayCommand(rawAlpha).number == 0.25f);

	// An event's type and the first 16 bytes of its data
	OcOverlayCommand event;
	event.kind = OcOverlayCommand::Event;
	event.eventType = vr::VREvent_MouseButtonDown;
	vr::VREvent_Data_t data{};
	data.mouse.x = 12.5f;
	data.mouse.y = 7.0f;
	data.mouse.button = vr::VRMouseButton_Left;
	memcpy(event.data, &data, sizeof(event.data));
	packed = OcPackOverlayCommand(event);
	REQUIRE(packed);
	uint64_t rawEvent[4] = { (*packed)[0], (*packed)[1], (*packed)[2], (*packed)[3] };
	back = OcUnpackOverlayCommand(rawEvent);
	CHECK(back.eventType == vr::VREvent_MouseButtonDown);
	vr::VREvent_Data_t got{};
	memcpy(&got, back.data, sizeof(back.data));
	CHECK(got.mouse.x == 12.5f);
	CHECK(got.mouse.y == 7.0f);
	CHECK(got.mouse.button == vr::VRMouseButton_Left);

	// A type too big to cross isn't packed
	event.eventType = 1u << 24;
	CHECK_FALSE(OcPackOverlayCommand(event));
}
#ifndef _WIN32
namespace {
// A child process that sleeps until it's ended, and its start time once it's running
pid_t SleepingChild(uint64_t& start)
{
	const pid_t child = fork();
	if (child == 0) {
		for (;;)
			pause();
	}
	start = 0;
	for (int i = 0; i < 200 && !start; i++) {
		start = SharedStore::ProcessStartTime((uint32_t)child);
		if (!start)
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return child;
}

bool EndedByKill(pid_t child)
{
	int status = 0;
	for (int i = 0; i < 400; i++) {
		if (waitpid(child, &status, WNOHANG) == child)
			return WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	kill(child, SIGKILL);
	waitpid(child, &status, 0);
	return false;
}
} // namespace

TEST_CASE("Kill_ends_only_the_process_that_started_then")
{
	uint64_t start = 0;
	const pid_t child = SleepingChild(start);
	REQUIRE(start != 0);
	// Another start time: a process that took the pid over, not ended
	CHECK_FALSE(SharedStore::KillProcess((uint32_t)child, start + 1));
	CHECK_FALSE(SharedStore::KillProcess((uint32_t)child, 0));
	CHECK(SharedStore::IsSameProcessAlive((uint32_t)child, start));
	CHECK(SharedStore::KillProcess((uint32_t)child, start));
	CHECK(EndedByKill(child));
}

TEST_CASE("Old_scene_application_ignoring_the_quit_is_ended_after_a_while")
{
	TempDir dir;
	SharedStore store(dir.path);
	BaseApplications apps(store);
	uint64_t start = 0;
	const pid_t child = SleepingChild(start);
	REQUIRE(start != 0);
	// Asked to quit for a launch just now: left running
	store.Update(StoreDoc::Scene, [&](Json::Value& doc) {
		doc["pid"] = (uint32_t)child;
		doc["start"] = (Json::UInt64)start;
		doc["transition"]["state"] = VRApplicationTransition_OldAppQuitSent;
		doc["transition"]["app_key"] = "oc.test.next";
		doc["transition"]["old_pid"] = (uint32_t)child;
		doc["transition"]["old_start"] = (Json::UInt64)start;
		doc["transition"]["launch"] = true;
		doc["transition"]["since_ms"] = NowMs();
	});
	CHECK(apps.GetTransitionState() == VRApplicationTransition_OldAppQuitSent);
	CHECK(SharedStore::IsSameProcessAlive((uint32_t)child, start));
	// Asked over ten seconds ago and still running: ended
	store.Update(StoreDoc::Scene, [&](Json::Value& doc) { doc["transition"]["since_ms"] = NowMs() - 11000; });
	apps.GetTransitionState();
	CHECK(EndedByKill(child));
}

TEST_CASE("Only_programs_the_store_knows_are_known_processes")
{
	TempDir dir;
	SharedStore store(dir.path);
	uint64_t start = 0;
	const pid_t child = SleepingChild(start);
	REQUIRE(start != 0);
	const std::string name = std::to_string(child);
	// Not in the store
	CHECK(store.KnownProcessStart((uint32_t)child) == 0);
	// Listed with another start time: a process that took the pid over
	store.Update(StoreDoc::Applications, [&](Json::Value& doc) {
		doc["pids"][name] = "oc.test.echo";
		doc["pid_starts"][name] = (Json::UInt64)(start + 1);
	});
	CHECK(store.KnownProcessStart((uint32_t)child) == 0);
	// Listed without one: not known well enough to end
	store.Update(StoreDoc::Applications, [&](Json::Value& doc) { doc["pid_starts"].removeMember(name); });
	CHECK(store.KnownProcessStart((uint32_t)child) == 0);
	// The application's, the scene's or an overlay program's start time
	store.Update(StoreDoc::Applications, [&](Json::Value& doc) { doc["pid_starts"][name] = (Json::UInt64)start; });
	CHECK(store.KnownProcessStart((uint32_t)child) == start);
	store.Update(StoreDoc::Applications, [&](Json::Value& doc) { doc = Json::Value(Json::objectValue); });
	store.Update(StoreDoc::Scene, [&](Json::Value& doc) {
		doc["pid"] = (uint32_t)child;
		doc["start"] = (Json::UInt64)start;
	});
	CHECK(store.KnownProcessStart((uint32_t)child) == start);
	store.Update(StoreDoc::Scene, [&](Json::Value& doc) { doc = Json::Value(Json::objectValue); });
	store.Update(StoreDoc::Overlays, [&](Json::Value& doc) { doc[name]["start"] = (Json::UInt64)start; });
	CHECK(store.KnownProcessStart((uint32_t)child) == start);
	kill(child, SIGKILL);
	int status = 0;
	waitpid(child, &status, 0);
	// Gone: not known any more
	CHECK(store.KnownProcessStart((uint32_t)child) == 0);
}
#endif
