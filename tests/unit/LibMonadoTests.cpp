// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

#include "Misc/ChaperoneData.h"
#include "Misc/LibMonado.h"
#include "Misc/PathUtf8.h"
#include "Misc/xrutil.h"
#include "Reimpl/BaseChaperoneSetup.h"
#include "XrController.h"

#ifndef _WIN32
#include <dlfcn.h>
#endif

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
void SetEnv(const char* name, const char* value)
{
#ifdef _WIN32
	_putenv_s(name, value ? value : "");
#else
	if (value)
		setenv(name, value, 1);
	else
		unsetenv(name);
#endif
}

// The fake library (tests/fakes/fake_libmonado.c) as libmonado, with its scripted answers, for one test
struct FakeLibMonado {
	FakeLibMonado(const char* major, const char* battery)
	{
		SetEnv("OC_TEST_LIBMONADO_PATH", OC_TEST_FAKE_LIBMONADO);
		SetEnv("FAKE_MND_MAJOR", major);
		SetEnv("FAKE_MND_BATTERY", battery);
	}
	~FakeLibMonado()
	{
		SetEnv("OC_TEST_LIBMONADO_PATH", nullptr);
		SetEnv("FAKE_MND_MAJOR", nullptr);
		SetEnv("FAKE_MND_BATTERY", nullptr);
		SetEnv("FAKE_MND_SAME_NAMES", nullptr);
		SetEnv("FAKE_MND_NO_BATTERY", nullptr);
		// Also those a test sets for part of it, in case a failed REQUIRE skipped its own reset
		SetEnv("FAKE_MND_NO_GET", nullptr);
		SetEnv("FAKE_MND_NO_SET", nullptr);
		SetEnv("FAKE_MND_NO_OFFSETS", nullptr);
	}
};
} // namespace

TEST_CASE("Battery_from_libmonado")
{
	FakeLibMonado fake("1", "1,1,0.5");
	LibMonado mnd("Monado(XRT) by Collabora et al");
	REQUIRE(mnd.Available());
	std::optional<LibMonado::Battery> b = mnd.BatteryFor("Fake Left", "");
	REQUIRE(b.has_value());
	CHECK(b->present);
	CHECK(b->charging);
	CHECK(b->charge == 0.5f);
	CHECK(mnd.BatteryFor("", "SN2").has_value()); // by serial
	CHECK_FALSE(mnd.BatteryFor("Nobody", "").has_value());
	CHECK(mnd.DeviceForRole("right") == std::optional<std::string>("Fake Right"));
	CHECK(mnd.BatteryForRole("head")->charge == 0.5f);
	CHECK_FALSE(mnd.DeviceForRole("gamepad").has_value());
}

TEST_CASE("Version_call_returning_nothing_accepted")
{
	// libmonado's mnd_api_get_version returns nothing, as the fake's does (built optimised, the fake's leaves its major
	// version, 1, in the return register): only the version it writes counts
	FakeLibMonado fake("1", "1,1,0.5");
	LibMonado mnd("Monado(XRT) by Collabora et al");
	CHECK(mnd.Available());
}

TEST_CASE("Wrong_major_version_unavailable")
{
	FakeLibMonado fake("2", "1,0,0.9");
	LibMonado mnd("Monado(XRT) by Collabora et al");
	CHECK_FALSE(mnd.Available());
	CHECK_FALSE(mnd.BatteryFor("Fake Left", "").has_value());
	CHECK_FALSE(mnd.BatteryForRole("left").has_value());
}

TEST_CASE("Missing_library_keeps_todays_answers")
{
	SetEnv("OC_TEST_LIBMONADO_PATH", (fs::temp_directory_path() / "oc_no_such_libmonado.so").string().c_str());
	LibMonado mnd("Monado(XRT) by Collabora et al");
	SetEnv("OC_TEST_LIBMONADO_PATH", nullptr);
	CHECK_FALSE(mnd.Available());
	CHECK_FALSE(mnd.BatteryForRole("left").has_value());
	vr::HmdMatrix34_t m{};
	CHECK_FALSE(mnd.SetReferenceSpaceOffset(0, m));
}

TEST_CASE("Non_monado_runtime_not_loaded")
{
	FakeLibMonado fake("1", "1,1,0.5");
	LibMonado mnd("SteamVR/OpenXR");
	CHECK_FALSE(mnd.Available());
}

TEST_CASE("Battery_by_role_is_the_roles_device")
{
	// Two controllers with one name (a driver may name them alike): each hand's battery is its own device's
	FakeLibMonado fake("1", "1,0,0.5;1,0,0.25;1,1,0.75");
	SetEnv("FAKE_MND_SAME_NAMES", "1");
	LibMonado mnd("Monado(XRT) by Collabora et al");
	REQUIRE(mnd.Available());
	std::optional<LibMonado::Battery> left = mnd.BatteryForRole("left"), right = mnd.BatteryForRole("right");
	SetEnv("FAKE_MND_SAME_NAMES", nullptr);
	REQUIRE(left.has_value());
	REQUIRE(right.has_value());
	CHECK(left->charge == 0.25f);
	CHECK(right->charge == 0.75f);
	CHECK(right->charging);
}

TEST_CASE("Battery_of_same_named_devices_goes_by_serial")
{
	// Two trackers of one model have one name: each one's battery is the device's with its serial
	FakeLibMonado fake("1", "1,0,0.5;1,0,0.25;1,1,0.75");
	SetEnv("FAKE_MND_SAME_NAMES", "1");
	LibMonado mnd("Monado(XRT) by Collabora et al");
	REQUIRE(mnd.Available());
	std::optional<LibMonado::Battery> second = mnd.BatteryFor("Fake Controller", "SN1"), third = mnd.BatteryFor("Fake Controller", "SN2");
	// A serial libmonado doesn't list: the first device of that name
	std::optional<LibMonado::Battery> byName = mnd.BatteryFor("Fake Controller", "SN9");
	SetEnv("FAKE_MND_SAME_NAMES", nullptr);
	REQUIRE(second.has_value());
	REQUIRE(third.has_value());
	REQUIRE(byName.has_value());
	CHECK(second->charge == 0.25f);
	CHECK(third->charge == 0.75f);
	CHECK(third->charging);
	CHECK(byName->charge == 0.5f);
}

TEST_CASE("Libmonado_is_asked_for_again_for_an_instance_made_again_at_the_same_handle")
{
	// A handle no runtime gave: the runtime's name can't be asked, so each is a LibMonado without a library
	const XrInstance before = xr_instance;
	xr_instance = (XrInstance)(uintptr_t)0x10;
	LibMonado* first = &LibMonado::Get();
	CHECK(&LibMonado::Get() == first);
	xr_instance_generation++;
	LibMonado* second = &LibMonado::Get();
	CHECK(second != first);
	CHECK(&LibMonado::Get() == second);
	xr_instance = before;
}

TEST_CASE("Battery_unsupported_is_no_battery")
{
	// Monado fails the call for a device without battery status: libmonado is there, and the device has none
	FakeLibMonado fake("1", "1,1,0.5");
	SetEnv("FAKE_MND_NO_BATTERY", "1");
	LibMonado mnd("Monado(XRT) by Collabora et al");
	std::optional<LibMonado::Battery> b = mnd.BatteryForRole("left");
	SetEnv("FAKE_MND_NO_BATTERY", nullptr);
	REQUIRE(mnd.Available());
	REQUIRE(b.has_value());
	CHECK_FALSE(b->present);
}

TEST_CASE("Libmonado_bare_name_loaded_by_name")
{
	// Monado's manifests may name the library without a folder (the soname distributions install): the loader's
	// search finds it, not the manifest's folder
	auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	fs::path dir = fs::temp_directory_path() / ("oc_mnd_bare_" + std::to_string(stamp));
	fs::create_directories(dir);
	fs::path manifest = dir / "openxr_monado.json";
	std::ofstream(manifest) << R"({ "file_format_version": "1.0.0", "runtime": { "library_path": "x.so", "MND_libmonado_path": "libmonado.so.1" } })";
	std::optional<fs::path> lib = LibMonado::LibraryFromManifest(manifest);
	std::error_code ec;
	fs::remove_all(dir, ec);
	REQUIRE(lib.has_value());
	CHECK(*lib == fs::path("libmonado.so.1"));
}

#ifdef _WIN32
TEST_CASE("Runtime_manifest_from_a_non_ascii_variable")
{
	// XR_RUNTIME_JSON with a user folder outside ASCII, read as the wide environment holds it
	const wchar_t* path = L"C:\\Users\\J\u00fcrgen\\monado\\openxr_monado.json";
	_wputenv_s(L"XR_RUNTIME_JSON", path);
	std::optional<fs::path> found = LibMonado::RuntimeManifestFromEnvironment();
	_wputenv_s(L"XR_RUNTIME_JSON", L"");
	REQUIRE(found.has_value());
	CHECK(found->wstring() == std::wstring(path));
}
#endif

TEST_CASE("Libmonado_path_from_manifest")
{
	auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	fs::path dir = fs::temp_directory_path() / ("oc_mnd_manifest_" + std::to_string(stamp));
	fs::create_directories(dir);
	fs::path manifest = dir / "openxr_monado.json";
	std::ofstream(manifest) << R"({ "file_format_version": "1.0.0", "runtime": { "name": "Monado", "library_path": "bin/openxr_monado.dll", "MND_libmonado_path": "bin/monado.dll" } })";
	std::optional<fs::path> lib = LibMonado::LibraryFromManifest(manifest);
	REQUIRE(lib.has_value());
	CHECK(*lib == (fs::weakly_canonical(dir) / "bin" / "monado.dll").lexically_normal());
	std::ofstream(manifest) << R"({ "file_format_version": "1.0.0", "runtime": { "library_path": "x.dll" } })";
	CHECK_FALSE(LibMonado::LibraryFromManifest(manifest).has_value());
	std::error_code ec;
	fs::remove_all(dir, ec);
}

TEST_CASE("Libmonado_beside_the_runtime_without_a_path")
{
	// A manifest naming only the runtime's library
	auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	fs::path dir = fs::weakly_canonical(fs::temp_directory_path()) / ("oc_mnd_beside_" + std::to_string(stamp));
	fs::create_directories(dir / "lib");
	fs::create_directories(dir / "manifests");
#ifdef _WIN32
	const char *runtimeName = "openxr_monado.dll", *libmonadoName = "monado.dll";
#else
	const char *runtimeName = "libopenxr_monado.so", *libmonadoName = "libmonado.so";
#endif
	fs::path manifest = dir / "manifests" / "active_runtime.json";
	auto write = [&](const std::string& libraryPath, const char* libmonadoPath = nullptr) {
		Json::Value root;
		root["file_format_version"] = "1.0.0";
		root["runtime"]["library_path"] = libraryPath;
		if (libmonadoPath)
			root["runtime"]["MND_libmonado_path"] = libmonadoPath;
		std::ofstream(manifest) << root.toStyledString();
	};
	write(PathToUtf8(dir / "lib" / runtimeName));
	const bool absentBefore = !LibMonado::LibraryFromManifest(manifest).has_value();
	std::ofstream(dir / "lib" / libmonadoName) << "";
	std::optional<fs::path> absolute = LibMonado::LibraryFromManifest(manifest);
	write(std::string("../lib/") + runtimeName);
	std::optional<fs::path> relative = LibMonado::LibraryFromManifest(manifest);
	write(runtimeName);
	const bool bareName = LibMonado::LibraryFromManifest(manifest).has_value();
	write(PathToUtf8(dir / "lib" / "other_runtime.so"));
	const bool otherRuntime = LibMonado::LibraryFromManifest(manifest).has_value();
	write(PathToUtf8(dir / "lib" / runtimeName), "");
	const bool emptyKey = LibMonado::LibraryFromManifest(manifest).has_value();
	std::error_code ec;
	fs::remove_all(dir, ec);
	CHECK(absentBefore);
	REQUIRE(absolute.has_value());
	CHECK(*absolute == dir / "lib" / libmonadoName);
	REQUIRE(relative.has_value());
	CHECK(*relative == dir / "lib" / libmonadoName);
	CHECK_FALSE(bareName);
	CHECK_FALSE(otherRuntime);
	CHECK_FALSE(emptyKey);
}

#ifndef _WIN32
TEST_CASE("Libmonado_path_through_a_chain_of_links")
{
	// active_runtime.json -> a link elsewhere -> the real manifest: the library is next to the real one
	auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	fs::path dir = fs::weakly_canonical(fs::temp_directory_path()) / ("oc_mnd_links_" + std::to_string(stamp));
	fs::create_directories(dir / "monado");
	fs::create_directories(dir / "config");
	fs::create_directories(dir / "alternatives");
	std::ofstream(dir / "monado" / "openxr_monado.json") << R"({ "file_format_version": "1.0.0", "runtime": { "library_path": "x.so", "MND_libmonado_path": "lib/libmonado.so" } })";
	fs::create_symlink("../monado/openxr_monado.json", dir / "alternatives" / "openxr");
	fs::create_symlink("../alternatives/openxr", dir / "config" / "active_runtime.json");
	std::optional<fs::path> lib = LibMonado::LibraryFromManifest(dir / "config" / "active_runtime.json");
	std::error_code ec;
	fs::remove_all(dir, ec);
	REQUIRE(lib.has_value());
	CHECK(*lib == dir / "monado" / "lib" / "libmonado.so");
}
#endif

namespace {
// One of the fake's call counts: it's the library LibMonado loaded (the same file, so the same module)
int FakeCount(const char* counter)
{
	using Fn = int (*)();
#ifdef _WIN32
	HMODULE lib = LoadLibraryA(OC_TEST_FAKE_LIBMONADO);
	Fn fn = lib ? (Fn)GetProcAddress(lib, counter) : nullptr;
#else
	void* lib = dlopen(OC_TEST_FAKE_LIBMONADO, RTLD_NOW);
	Fn fn = lib ? (Fn)dlsym(lib, counter) : nullptr;
#endif
	REQUIRE(fn);
	return fn();
}
int FakeBatteryCalls() { return FakeCount("fake_mnd_battery_calls"); }
} // namespace

TEST_CASE("Bool_property_makes_no_battery_call")
{
	FakeLibMonado fake("1", "1,1,0.5");
	LibMonado mnd("Monado(XRT) by Collabora et al");
	REQUIRE(mnd.Available());

	int before = FakeBatteryCalls();
	CHECK_FALSE(XrController::BatteryBoolProperty(mnd, vr::Prop_DeviceIsWireless_Bool, "left").has_value());
	CHECK_FALSE(XrController::BatteryBoolProperty(mnd, vr::Prop_DeviceCanPowerOff_Bool, "right").has_value());
	CHECK(FakeBatteryCalls() == before);

	// The battery's own properties ask for it (LibMonado keeps the answer a while, so once here)
	CHECK(XrController::BatteryBoolProperty(mnd, vr::Prop_DeviceProvidesBatteryStatus_Bool, "left") == std::optional<bool>(true));
	CHECK(XrController::BatteryBoolProperty(mnd, vr::Prop_DeviceIsCharging_Bool, "left") == std::optional<bool>(true));
	int asked = FakeBatteryCalls();
	CHECK(asked > before);
	// No role: nothing asked, and no battery claimed
	CHECK(XrController::BatteryBoolProperty(mnd, vr::Prop_DeviceProvidesBatteryStatus_Bool, nullptr) == std::optional<bool>(false));
	CHECK(FakeBatteryCalls() == asked);
}

TEST_CASE("No_battery_claimed_without_libmonado")
{
	// A runtime other than Monado has none: the controller doesn't say it provides a battery it can't read
	LibMonado none("Some other runtime");
	REQUIRE_FALSE(none.Available());
	CHECK(XrController::BatteryBoolProperty(none, vr::Prop_DeviceProvidesBatteryStatus_Bool, "left") == std::optional<bool>(false));
	CHECK_FALSE(XrController::BatteryBoolProperty(none, vr::Prop_DeviceIsCharging_Bool, "left").has_value());
}

TEST_CASE("Device_name_and_serial_by_role")
{
	FakeLibMonado fake("1", "1,1,0.5");
	LibMonado mnd("Monado(XRT) by Collabora et al");
	REQUIRE(mnd.Available());
	std::optional<LibMonado::DeviceInfo> head = mnd.DeviceInfoForRole("head");
	REQUIRE(head.has_value());
	CHECK(head->name == "Fake HMD");
	CHECK(head->serial == "SN0");
	CHECK(mnd.DeviceInfoForRole("right")->serial == "SN2");
	CHECK_FALSE(mnd.DeviceInfoForRole("gamepad").has_value());
	LibMonado none("Some other runtime");
	CHECK_FALSE(none.DeviceInfoForRole("head").has_value());
}

TEST_CASE("Battery_of_a_named_device_is_kept_a_while")
{
	FakeLibMonado fake("1", "1,0,0.25");
	LibMonado mnd("Monado(XRT) by Collabora et al");
	REQUIRE(mnd.Available());
	std::optional<LibMonado::Battery> first = mnd.BatteryFor("Fake Right", "SN2");
	REQUIRE(first.has_value());
	int asked = FakeBatteryCalls();
	CHECK(mnd.BatteryFor("Fake Right", "SN2")->charge == 0.25f);
	CHECK(FakeBatteryCalls() == asked);
}

#ifndef _WIN32
TEST_CASE("Manifest_search_xdg_config_dirs")
{
	// No XR_RUNTIME_JSON, an XDG_CONFIG_HOME without one, and XDG_CONFIG_DIRS's second folder with one: that one, as
	// the loader finds it
	auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	fs::path root = fs::temp_directory_path() / ("oc_xdg_" + std::to_string(stamp));
	fs::create_directories(root / "home");
	fs::create_directories(root / "first");
	fs::create_directories(root / "second" / "openxr" / "1");
	fs::path manifest = root / "second" / "openxr" / "1" / "active_runtime.json";
	std::ofstream(manifest) << "{}";

	auto saved = [](const char* name) { const char* v = getenv(name); return v ? std::optional<std::string>(v) : std::nullopt; };
	std::optional<std::string> runtimeJson = saved("XR_RUNTIME_JSON"), home = saved("XDG_CONFIG_HOME"), dirs = saved("XDG_CONFIG_DIRS");
	SetEnv("XR_RUNTIME_JSON", nullptr);
	SetEnv("XDG_CONFIG_HOME", (root / "home").c_str());
	SetEnv("XDG_CONFIG_DIRS", ((root / "first").string() + ":" + (root / "second").string()).c_str());
	std::optional<fs::path> found = LibMonado::ActiveRuntimeManifest();
	SetEnv("XR_RUNTIME_JSON", runtimeJson ? runtimeJson->c_str() : nullptr);
	SetEnv("XDG_CONFIG_HOME", home ? home->c_str() : nullptr);
	SetEnv("XDG_CONFIG_DIRS", dirs ? dirs->c_str() : nullptr);
	std::error_code ec;
	fs::remove_all(root, ec);

	REQUIRE(found.has_value());
	CHECK(*found == manifest);
}
#endif

namespace {
// The fake's log of reference space offset calls (FAKE_MND_LOG), one line each, for one test
struct OffsetLog {
	fs::path path = fs::temp_directory_path() / ("oc_fake_mnd_offsets_" + std::to_string(std::random_device {}()) + ".log");
	OffsetLog()
	{
		fs::remove(path);
		SetEnv("FAKE_MND_LOG", path.string().c_str());
	}
	~OffsetLog()
	{
		SetEnv("FAKE_MND_LOG", nullptr);
		fs::remove(path);
	}
	std::vector<std::string> Lines() const
	{
		std::vector<std::string> lines;
		std::ifstream in(path);
		for (std::string line; std::getline(in, line);)
			lines.push_back(line);
		return lines;
	}
};

// Whether a logged call moved that reference space to (px, py, pz, qx, qy, qz, qw), to a millionth
bool Moved(const std::string& line, int type, std::vector<float> pose)
{
	std::istringstream in(line);
	std::string call;
	int got = -1;
	in >> call >> got;
	if (call != "set_reference_space_offset" || got != type)
		return false;
	for (float want : pose) {
		float v = 0;
		if (!(in >> v) || std::fabs(v - want) > 1e-6f)
			return false;
	}
	return true;
}

// The fake's offsets back to identity, as when Monado's service restarts
void RestartMonado() { FakeCount("fake_mnd_restart"); }

constexpr int kStage = LibMonado::kReferenceSpaceStage;
constexpr float kHalf = 0.70710677f; // the quaternion's parts for a 90 degree turn

// A room setup with its standing zero pose 1 m forward (along -Z) and turned 90 degrees left
ChaperoneData RoomOneMetreForward()
{
	ChaperoneData d = ChaperoneData::FromStage(2, 3);
	d.standingZero = ChaperoneData::Identity();
	d.standingZero.m[0][0] = d.standingZero.m[2][2] = 0;
	d.standingZero.m[0][2] = 1;
	d.standingZero.m[2][0] = -1;
	d.standingZero.m[2][3] = -1;
	return d;
}

// The same with the zero pose 2 m forward, not turned
ChaperoneData RoomTwoMetresForward()
{
	ChaperoneData d = ChaperoneData::FromStage(2, 3);
	d.standingZero = ChaperoneData::Identity();
	d.standingZero.m[2][3] = -2;
	return d;
}

// An offset 0.5 m up, as another program might set it
vr::HmdMatrix34_t HalfMetreUp()
{
	vr::HmdMatrix34_t m = ChaperoneData::Identity();
	m.m[1][3] = 0.5f;
	return m;
}
} // namespace

TEST_CASE("Commit_standing_zero_applies_stage_offset")
{
	FakeLibMonado fake("1", "0,0,0");
	OffsetLog log;
	LibMonado mnd("Monado(XRT) by Collabora et al");
	REQUIRE(mnd.Available());
	RestartMonado();
	ChaperoneData room = RoomOneMetreForward();
	Json::Value offsets(Json::objectValue);
	std::optional<vr::HmdMatrix34_t> before = BaseChaperoneSetup::ApplyZeroPoses(offsets, room, mnd);
	// STAGE (3) at the pose: 1 m along -Z, turned 90 degrees about +Y
	std::vector<std::string> lines = log.Lines();
	REQUIRE(lines.size() == 1);
	CHECK(Moved(lines[0], kStage, { 0, 0, -1, 0, kHalf, 0, kHalf }));
	// The offset it had before, for putting back
	REQUIRE(before.has_value());
	CHECK(before->m[2][3] == 0.0f);
	// Recorded: the zero pose and the absolute offset that set
	CHECK(ChaperoneData::MatrixFromJson(offsets["stage"]["zero"]).m[2][3] == -1.0f);
	CHECK(ChaperoneData::MatrixFromJson(offsets["stage"]["offset"]).m[2][3] == doctest::Approx(-1));
	// The offset as libmonado gives it back
	std::optional<vr::HmdMatrix34_t> now = mnd.GetReferenceSpaceOffset(kStage);
	REQUIRE(now.has_value());
	CHECK(now->m[2][3] == doctest::Approx(-1));
	CHECK(now->m[0][2] == doctest::Approx(1));

	// Back to the runtime's own stage
	ChaperoneData back = room;
	back.standingZero = ChaperoneData::Identity();
	BaseChaperoneSetup::ApplyZeroPoses(offsets, back, mnd);
	lines = log.Lines();
	REQUIRE(lines.size() == 2);
	CHECK(Moved(lines[1], kStage, { 0, 0, 0, 0, 0, 0, 1 }));
}

TEST_CASE("Commit_twice_applies_once")
{
	FakeLibMonado fake("1", "0,0,0");
	OffsetLog log;
	ChaperoneData room = RoomOneMetreForward();
	Json::Value offsets(Json::objectValue);
	{
		LibMonado mnd("Monado(XRT) by Collabora et al");
		REQUIRE(mnd.Available());
		RestartMonado();
		BaseChaperoneSetup::ApplyZeroPoses(offsets, room, mnd);
		CHECK_FALSE(BaseChaperoneSetup::ApplyZeroPoses(offsets, room, mnd).has_value());
	}
	// Another program committing the same room, with the document as the first one left it
	{
		LibMonado mnd("Monado(XRT) by Collabora et al");
		BaseChaperoneSetup::ApplyZeroPoses(offsets, room, mnd);
	}
	std::vector<std::string> lines = log.Lines();
	REQUIRE(lines.size() == 1);
	CHECK(Moved(lines[0], kStage, { 0, 0, -1, 0, kHalf, 0, kHalf }));

	// Back to the runtime's own stage
	LibMonado mnd("Monado(XRT) by Collabora et al");
	BaseChaperoneSetup::ApplyZeroPoses(offsets, ChaperoneData::FromStage(2, 3), mnd);
	lines = log.Lines();
	REQUIRE(lines.size() == 2);
	CHECK(Moved(lines[1], kStage, { 0, 0, 0, 0, 0, 0, 1 }));
}

TEST_CASE("Commit_after_a_restart_of_Monado")
{
	// Monado keeps the offsets only while its service runs: after a restart the stage is its own again, and the
	// document still says what was applied before
	FakeLibMonado fake("1", "0,0,0");
	OffsetLog log;
	LibMonado mnd("Monado(XRT) by Collabora et al");
	REQUIRE(mnd.Available());
	RestartMonado();
	Json::Value offsets(Json::objectValue);
	BaseChaperoneSetup::ApplyZeroPoses(offsets, RoomOneMetreForward(), mnd);

	// The same room again: applied again, not taken as already there, and not twice
	RestartMonado();
	BaseChaperoneSetup::ApplyZeroPoses(offsets, RoomOneMetreForward(), mnd);
	// A new room: onto the runtime's own stage, not relative to the one applied before the restart
	RestartMonado();
	BaseChaperoneSetup::ApplyZeroPoses(offsets, RoomTwoMetresForward(), mnd);
	// And without a restart, from that one to the first again
	BaseChaperoneSetup::ApplyZeroPoses(offsets, RoomOneMetreForward(), mnd);

	std::vector<std::string> lines = log.Lines();
	REQUIRE(lines.size() == 4);
	CHECK(Moved(lines[0], kStage, { 0, 0, -1, 0, kHalf, 0, kHalf }));
	CHECK(Moved(lines[1], kStage, { 0, 0, -1, 0, kHalf, 0, kHalf }));
	CHECK(Moved(lines[2], kStage, { 0, 0, -2, 0, 0, 0, 1 }));
	CHECK(Moved(lines[3], kStage, { 0, 0, -1, 0, kHalf, 0, kHalf }));
	RestartMonado();
}

TEST_CASE("Commit_onto_an_offset_without_a_record")
{
	// The stage moved by another program (or a document without the record): its offset now is the base
	FakeLibMonado fake("1", "0,0,0");
	OffsetLog log;
	LibMonado mnd("Monado(XRT) by Collabora et al");
	REQUIRE(mnd.Available());
	RestartMonado();
	REQUIRE(mnd.SetReferenceSpaceOffset(kStage, HalfMetreUp()));
	Json::Value offsets(Json::objectValue);
	BaseChaperoneSetup::ApplyZeroPoses(offsets, RoomOneMetreForward(), mnd);
	std::vector<std::string> lines = log.Lines();
	REQUIRE(lines.size() == 2);
	CHECK(Moved(lines[1], kStage, { 0, 0.5f, -1, 0, kHalf, 0, kHalf }));

	// A record whose offset isn't the one now (another program moved it since): the same
	RestartMonado();
	REQUIRE(mnd.SetReferenceSpaceOffset(kStage, HalfMetreUp()));
	BaseChaperoneSetup::ApplyZeroPoses(offsets, RoomTwoMetresForward(), mnd);
	lines = log.Lines();
	REQUIRE(lines.size() == 4);
	CHECK(Moved(lines[3], kStage, { 0, 0.5f, -2, 0, 0, 0, 1 }));
	RestartMonado();
}

TEST_CASE("Commit_without_the_get_call_sets_the_zero_pose")
{
	// libmonado without mnd_root_get_reference_space_offset: the zero pose is the absolute offset, on every commit
	FakeLibMonado fake("1", "0,0,0");
	OffsetLog log;
	LibMonado mnd("Monado(XRT) by Collabora et al");
	REQUIRE(mnd.Available());
	RestartMonado();
	REQUIRE(mnd.SetReferenceSpaceOffset(kStage, HalfMetreUp()));
	SetEnv("FAKE_MND_NO_GET", "1");
	Json::Value offsets(Json::objectValue);
	CHECK_FALSE(BaseChaperoneSetup::ApplyZeroPoses(offsets, RoomOneMetreForward(), mnd).has_value());
	BaseChaperoneSetup::ApplyZeroPoses(offsets, RoomOneMetreForward(), mnd);
	SetEnv("FAKE_MND_NO_GET", nullptr);
	std::vector<std::string> lines = log.Lines();
	REQUIRE(lines.size() == 3);
	CHECK(Moved(lines[1], kStage, { 0, 0, -1, 0, kHalf, 0, kHalf }));
	CHECK(Moved(lines[2], kStage, { 0, 0, -1, 0, kHalf, 0, kHalf }));
	CHECK(ChaperoneData::MatrixFromJson(offsets["stage"]["offset"]).m[2][3] == -1.0f);
	RestartMonado();
}

TEST_CASE("No_libmonado_keeps_offsets_only")
{
	OffsetLog log;
	SetEnv("OC_TEST_LIBMONADO_PATH", "no-such-libmonado.dll");
	LibMonado mnd("Monado(XRT) by Collabora et al");
	SetEnv("OC_TEST_LIBMONADO_PATH", nullptr);
	REQUIRE_FALSE(mnd.Available());
	CHECK_FALSE(mnd.GetReferenceSpaceOffset(kStage).has_value());
	ChaperoneData room = RoomOneMetreForward();
	Json::Value offsets(Json::objectValue);
	CHECK_FALSE(BaseChaperoneSetup::ApplyZeroPoses(offsets, room, mnd).has_value());
	CHECK(log.Lines().empty());
	CHECK(offsets.empty());

	// libmonado refusing it: nothing recorded, so the next commit tries again
	FakeLibMonado fake("1", "0,0,0");
	SetEnv("FAKE_MND_NO_OFFSETS", "1");
	{
		LibMonado refusing("Monado(XRT) by Collabora et al");
		REQUIRE(refusing.Available());
		CHECK_FALSE(BaseChaperoneSetup::ApplyZeroPoses(offsets, room, refusing).has_value());
		CHECK_FALSE(BaseChaperoneSetup::ApplyZeroPoses(offsets, room, refusing).has_value());
	}
	SetEnv("FAKE_MND_NO_OFFSETS", nullptr);
	CHECK(offsets.empty());
}

namespace {
// A head's pose in the stage: at a place, turned left by a yaw (degrees), then tipped up by a pitch
vr::HmdMatrix34_t HeadAt(float x, float y, float z, float yawDegrees, float pitchDegrees = 0)
{
	const float yaw = yawDegrees * 3.14159265f / 180, pitch = pitchDegrees * 3.14159265f / 180;
	const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
	// Ry(yaw) * Rx(pitch)
	vr::HmdMatrix34_t m{};
	m.m[0][0] = cy, m.m[0][1] = sy * sp, m.m[0][2] = sy * cp, m.m[0][3] = x;
	m.m[1][0] = 0, m.m[1][1] = cp, m.m[1][2] = -sp, m.m[1][3] = y;
	m.m[2][0] = -sy, m.m[2][1] = cy * sp, m.m[2][2] = cy * cp, m.m[2][3] = z;
	return m;
}

void CheckYawOnly(const vr::HmdMatrix34_t& m, float x, float z, float yawDegrees)
{
	const float yaw = yawDegrees * 3.14159265f / 180;
	CHECK(m.m[0][3] == doctest::Approx(x));
	CHECK(m.m[1][3] == doctest::Approx(0)); // on the floor
	CHECK(m.m[2][3] == doctest::Approx(z));
	// Up stays up
	CHECK(m.m[1][1] == doctest::Approx(1));
	CHECK(m.m[0][1] == doctest::Approx(0));
	CHECK(m.m[2][1] == doctest::Approx(0));
	CHECK(m.m[1][0] == doctest::Approx(0));
	CHECK(m.m[1][2] == doctest::Approx(0));
	CHECK(m.m[0][0] == doctest::Approx(std::cos(yaw)));
	CHECK(m.m[0][2] == doctest::Approx(std::sin(yaw)));
	CHECK(m.m[2][0] == doctest::Approx(-std::sin(yaw)));
	CHECK(m.m[2][2] == doctest::Approx(std::cos(yaw)));
}
} // namespace

TEST_CASE("Recentred_origin_is_under_the_head_turned_about_the_vertical")
{
	// Level, facing -Z: under the head, not turned
	CheckYawOnly(BaseChaperoneSetup::RecentredOrigin(HeadAt(0.5f, 1.7f, -0.25f, 0)), 0.5f, -0.25f, 0);
	// Turned 90 degrees left
	CheckYawOnly(BaseChaperoneSetup::RecentredOrigin(HeadAt(1, 1.2f, 2, 90)), 1, 2, 90);
	// Turned and looking 40 degrees up, or 60 down: the turn alone
	CheckYawOnly(BaseChaperoneSetup::RecentredOrigin(HeadAt(-1, 1.7f, 0.5f, -30, 40)), -1, 0.5f, -30);
	CheckYawOnly(BaseChaperoneSetup::RecentredOrigin(HeadAt(0, 1.7f, 0, 135, -60)), 0, 0, 135);
	// Looking straight down or up: the way the head's top, or its bottom, points
	CheckYawOnly(BaseChaperoneSetup::RecentredOrigin(HeadAt(0, 1.7f, 0, 45, -90)), 0, 0, 45);
	CheckYawOnly(BaseChaperoneSetup::RecentredOrigin(HeadAt(0, 1.7f, 0, 45, 90)), 0, 0, 45);
	// Tilted to the side too (a roll of 20 degrees after a turn of 90)
	vr::HmdMatrix34_t rolled = HeadAt(0, 1.7f, 0, 90);
	const float c = std::cos(0.349f), s = std::sin(0.349f);
	for (int r = 0; r < 3; r++) {
		const float x = rolled.m[r][0], y = rolled.m[r][1];
		rolled.m[r][0] = x * c + y * s;
		rolled.m[r][1] = y * c - x * s;
	}
	CheckYawOnly(BaseChaperoneSetup::RecentredOrigin(rolled), 0, 0, 90);
}

TEST_CASE("Recentre_moves_the_stage_on_top_of_its_offset")
{
	FakeLibMonado fake("1", "0,0,0");
	OffsetLog log;
	LibMonado mnd("Monado(XRT) by Collabora et al");
	REQUIRE(mnd.Available());
	RestartMonado();
	Json::Value offsets(Json::objectValue);
	// The head 1 m right of the stage's origin, 1.7 m up, turned 90 degrees left
	const vr::HmdMatrix34_t origin = BaseChaperoneSetup::RecentredOrigin(HeadAt(1, 1.7f, 0, 90));
	std::optional<vr::HmdMatrix34_t> before = BaseChaperoneSetup::MoveStage(offsets, origin, mnd);
	REQUIRE(before.has_value());
	CHECK(before->m[0][3] == 0.0f);
	std::vector<std::string> lines = log.Lines();
	REQUIRE(lines.size() == 1);
	CHECK(Moved(lines[0], kStage, { 1, 0, 0, 0, kHalf, 0, kHalf }));
	// Nothing was recorded before, and nothing is: no zero pose is applied
	CHECK(offsets.empty());

	// Again, with the head now 1 m ahead of the new origin (along its -Z, the room's -X), not turned: on top of the
	// first, not in its place
	BaseChaperoneSetup::MoveStage(offsets, BaseChaperoneSetup::RecentredOrigin(HeadAt(0, 1.7f, -1, 0)), mnd);
	lines = log.Lines();
	REQUIRE(lines.size() == 2);
	CHECK(Moved(lines[1], kStage, { 0, 0, 0, 0, kHalf, 0, kHalf }));
	RestartMonado();
}

TEST_CASE("Recentre_keeps_an_offset_set_by_another_program")
{
	// The stage 0.5 m up when the game starts (another program's offset): the floor stays where that put it
	FakeLibMonado fake("1", "0,0,0");
	OffsetLog log;
	LibMonado mnd("Monado(XRT) by Collabora et al");
	REQUIRE(mnd.Available());
	RestartMonado();
	REQUIRE(mnd.SetReferenceSpaceOffset(kStage, HalfMetreUp()));
	Json::Value offsets(Json::objectValue);
	BaseChaperoneSetup::MoveStage(offsets, BaseChaperoneSetup::RecentredOrigin(HeadAt(0, 1.7f, -2, 0, 30)), mnd);
	std::vector<std::string> lines = log.Lines();
	REQUIRE(lines.size() == 2);
	CHECK(Moved(lines[1], kStage, { 0, 0.5f, -2, 0, 0, 0, 1 }));
	RestartMonado();
}

TEST_CASE("Recentre_after_a_committed_zero_pose")
{
	FakeLibMonado fake("1", "0,0,0");
	OffsetLog log;
	LibMonado mnd("Monado(XRT) by Collabora et al");
	REQUIRE(mnd.Available());
	RestartMonado();
	Json::Value offsets(Json::objectValue);
	// A room setup's zero pose 2 m forward, then a recentre with the head 1 m to the right of it
	BaseChaperoneSetup::ApplyZeroPoses(offsets, RoomTwoMetresForward(), mnd);
	BaseChaperoneSetup::MoveStage(offsets, BaseChaperoneSetup::RecentredOrigin(HeadAt(1, 1.7f, 0, 0)), mnd);
	std::vector<std::string> lines = log.Lines();
	REQUIRE(lines.size() == 2);
	CHECK(Moved(lines[1], kStage, { 1, 0, -2, 0, 0, 0, 1 }));
	// The record follows: the same zero pose, the offset as recentred
	CHECK(ChaperoneData::MatrixFromJson(offsets["stage"]["zero"]).m[2][3] == -2.0f);
	CHECK(ChaperoneData::MatrixFromJson(offsets["stage"]["offset"]).m[0][3] == doctest::Approx(1));

	// The same room committed again moves nothing: the recentre stays
	CHECK_FALSE(BaseChaperoneSetup::ApplyZeroPoses(offsets, RoomTwoMetresForward(), mnd).has_value());
	CHECK(log.Lines().size() == 2);
	// Another room replaces the zero pose and keeps the recentre
	ChaperoneData threeForward = RoomTwoMetresForward();
	threeForward.standingZero.m[2][3] = -3;
	BaseChaperoneSetup::ApplyZeroPoses(offsets, threeForward, mnd);
	lines = log.Lines();
	REQUIRE(lines.size() == 3);
	CHECK(Moved(lines[2], kStage, { 1, 0, -3, 0, 0, 0, 1 }));
	RestartMonado();
}

TEST_CASE("Recentre_without_libmonado_or_its_get_call_moves_nothing")
{
	OffsetLog log;
	const vr::HmdMatrix34_t origin = BaseChaperoneSetup::RecentredOrigin(HeadAt(1, 1.7f, 0, 90));
	Json::Value offsets(Json::objectValue);
	SetEnv("OC_TEST_LIBMONADO_PATH", "no-such-libmonado.dll");
	{
		LibMonado none("Monado(XRT) by Collabora et al");
		REQUIRE_FALSE(none.Available());
		CHECK_FALSE(BaseChaperoneSetup::MoveStage(offsets, origin, none).has_value());
	}
	SetEnv("OC_TEST_LIBMONADO_PATH", nullptr);

	// Without the get call the offset now isn't known, so there's nothing to put the recentre on top of
	FakeLibMonado fake("1", "0,0,0");
	LibMonado mnd("Monado(XRT) by Collabora et al");
	REQUIRE(mnd.Available());
	RestartMonado();
	SetEnv("FAKE_MND_NO_GET", "1");
	CHECK_FALSE(BaseChaperoneSetup::MoveStage(offsets, origin, mnd).has_value());
	SetEnv("FAKE_MND_NO_GET", nullptr);
	// Monado giving the offset and refusing the new one
	SetEnv("FAKE_MND_NO_SET", "1");
	CHECK_FALSE(BaseChaperoneSetup::MoveStage(offsets, origin, mnd).has_value());
	SetEnv("FAKE_MND_NO_SET", nullptr);
	// Monado refusing both calls
	SetEnv("FAKE_MND_NO_OFFSETS", "1");
	{
		LibMonado refusing("Monado(XRT) by Collabora et al");
		REQUIRE(refusing.Available());
		CHECK_FALSE(BaseChaperoneSetup::MoveStage(offsets, origin, refusing).has_value());
	}
	SetEnv("FAKE_MND_NO_OFFSETS", nullptr);
	CHECK(log.Lines().empty());
	CHECK(offsets.empty());
}
