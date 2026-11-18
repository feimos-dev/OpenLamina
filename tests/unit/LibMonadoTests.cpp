// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

#include "Misc/ChaperoneData.h"
#include "Misc/LibMonado.h"
#include "Reimpl/BaseChaperoneSetup.h"
#include "XrController.h"

#ifndef _WIN32
#include <dlfcn.h>
#endif

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
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
	// No role (a tracker): nothing asked
	CHECK(XrController::BatteryBoolProperty(mnd, vr::Prop_DeviceProvidesBatteryStatus_Bool, nullptr) == std::optional<bool>(true));
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
	fs::path path = fs::temp_directory_path() / "oc_fake_mnd_offsets.log";
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
