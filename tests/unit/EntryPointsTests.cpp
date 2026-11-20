// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#ifndef _WIN32

#include "stdafx.h"

#include "Reimpl/BaseClientCore.h"
#include "steamvr_abi.h"

#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

// The exports of the runtime library, loaded as a program would load it
struct Runtime {
	void* lib = dlopen(OC_TEST_VRCLIENT, RTLD_NOW | RTLD_LOCAL);

	template <typename F>
	F fn(const char* name)
	{
		return lib ? reinterpret_cast<F>(dlsym(lib, name)) : nullptr;
	}

	~Runtime()
	{
		if (lib)
			dlclose(lib);
	}
};

using InitFn = uint32_t (*)(vr::EVRInitError*, vr::EVRApplicationType, const char*);
using ShutdownFn = void (*)();
using TokenFn = uint32_t (*)();
using GenericFn = void* (*)(const char*, vr::EVRInitError*);
using RuntimePathFn = bool (*)(char*, uint32_t, uint32_t*);

} // namespace

TEST_CASE("The init token changes with every session")
{
	Runtime rt;
	auto init = rt.fn<InitFn>("VR_InitInternal2");
	auto shutdown = rt.fn<ShutdownFn>("VR_ShutdownInternal");
	auto token = rt.fn<TokenFn>("VR_GetInitToken");
	REQUIRE(init);
	REQUIRE(shutdown);
	REQUIRE(token);

	vr::EVRInitError err = vr::VRInitError_Unknown;
	uint32_t first = init(&err, vr::VRApplication_Utility, nullptr);
	REQUIRE(err == vr::VRInitError_None);
	CHECK(first != 0);
	CHECK(token() == first);
	shutdown();

	uint32_t second = init(&err, vr::VRApplication_Utility, nullptr);
	REQUIRE(err == vr::VRInitError_None);
	CHECK(second != 0);
	CHECK(second == token());
	CHECK(second != first);
	shutdown();
}

TEST_CASE("A refused Init's Shutdown keeps another caller's session")
{
	Runtime rt;
	auto init = rt.fn<InitFn>("VR_InitInternal2");
	auto shutdown = rt.fn<ShutdownFn>("VR_ShutdownInternal");
	auto generic = rt.fn<GenericFn>("VR_GetGenericInterface");
	REQUIRE(init);
	REQUIRE(shutdown);
	REQUIRE(generic);

	vr::EVRInitError err = vr::VRInitError_None;
	REQUIRE(init(&err, vr::VRApplication_Utility, nullptr) != 0);
	REQUIRE(err == vr::VRInitError_None);

	// Refused (the Bootstrapper type is never served), then the caller shuts down as the header's VR_Init does
	CHECK(init(&err, vr::VRApplication_Bootstrapper, nullptr) == 0);
	CHECK(err != vr::VRInitError_None);
	shutdown();

	// The first caller's session is still there
	err = vr::VRInitError_Unknown;
	CHECK(generic("IVRApplications_007", &err) != nullptr);
	CHECK(err == vr::VRInitError_None);

	shutdown();
	err = vr::VRInitError_None;
	CHECK(generic("IVRApplications_007", &err) == nullptr);
	CHECK(err != vr::VRInitError_None);
}

TEST_CASE("A Utility app has the settings interface")
{
	Runtime rt;
	auto init = rt.fn<InitFn>("VR_InitInternal2");
	auto shutdown = rt.fn<ShutdownFn>("VR_ShutdownInternal");
	auto generic = rt.fn<GenericFn>("VR_GetGenericInterface");
	REQUIRE(init);
	REQUIRE(shutdown);
	REQUIRE(generic);

	vr::EVRInitError err = vr::VRInitError_None;
	REQUIRE(init(&err, vr::VRApplication_Utility, nullptr) != 0);

	err = vr::VRInitError_Unknown;
	CHECK(generic("IVRSettings_003", &err) != nullptr);
	CHECK(err == vr::VRInitError_None);

	// Interfaces that need the headset stay out of its reach
	err = vr::VRInitError_None;
	CHECK(generic("IVRCompositor_028", &err) == nullptr);
	CHECK(err == vr::VRInitError_Init_NotAvailableToUtilityApps);
	shutdown();
}

TEST_CASE("VR_GetRuntimePath fills the caller's buffer")
{
	Runtime rt;
	auto path = rt.fn<RuntimePathFn>("VR_GetRuntimePath");
	auto legacy = rt.fn<const char* (*)()>("VR_RuntimePath");
	REQUIRE(path);
	REQUIRE(legacy);

	const std::string expected = legacy();
	uint32_t needed = 0;
	CHECK_FALSE(path(nullptr, 0, &needed));
	CHECK(needed == expected.size() + 1);

	// One byte short: nothing fits, and the needed size is still reported
	std::string buffer(expected.size(), 'x');
	needed = 0;
	CHECK_FALSE(path(buffer.data(), (uint32_t)buffer.size(), &needed));
	CHECK(needed == expected.size() + 1);

	buffer.assign(needed, 'x');
	CHECK(path(buffer.data(), needed, &needed));
	CHECK(std::string(buffer.c_str()) == expected);

	// The required size is optional
	CHECK(path(buffer.data(), (uint32_t)buffer.size(), nullptr));
}

TEST_CASE("Interface versions are valid only where they exist")
{
	Runtime rt;
	auto valid = rt.fn<bool (*)(const char*)>("VR_IsInterfaceVersionValid");
	REQUIRE(valid);
	CHECK(valid("IVRSettings_003"));
	CHECK(valid("FnTable:IVRSettings_003"));
	CHECK(valid("IVRSystem_022"));
	CHECK_FALSE(valid("IVRNotAnInterface_001"));
	CHECK_FALSE(valid("FnTable:IVRNotAnInterface_001"));
	CHECK_FALSE(valid(""));
	CHECK_FALSE(valid(nullptr));
}

TEST_CASE("Init errors have text of their own")
{
	Runtime rt;
	auto describe = rt.fn<const char* (*)(vr::EVRInitError)>("VR_GetVRInitErrorAsEnglishDescription");
	REQUIRE(describe);
	CHECK(std::string(describe(vr::VRInitError_None)) == "None");
	CHECK(std::string(describe((vr::EVRInitError)147)) == "VRInitError_Init_IllegalTypeTransition");
	CHECK(std::string(describe((vr::EVRInitError)200)).find("IllegalTypeTransition") == std::string::npos);
	CHECK(std::string(describe((vr::EVRInitError)99999)) == "Unknown error (99999)");
}

namespace {
struct EnvGuard {
	std::string name;
	std::string old;
	bool had;
	EnvGuard(const char* n, const char* value)
	    : name(n)
	{
		const char* o = getenv(n);
		had = o != nullptr;
		if (o)
			old = o;
		if (value)
			setenv(n, value, 1);
		else
			unsetenv(n);
	}
	~EnvGuard()
	{
		if (had)
			setenv(name.c_str(), old.c_str(), 1);
		else
			unsetenv(name.c_str());
	}
};

// A config folder with an openvrpaths.vrpath naming one runtime
std::filesystem::path MakeConfig(const std::filesystem::path& configRoot, const std::string& runtime)
{
	std::filesystem::create_directories(configRoot / "openvr");
	std::ofstream(configRoot / "openvr" / "openvrpaths.vrpath") << "{\"runtime\": [\"" << runtime << "\"]}";
	return configRoot;
}
} // namespace

TEST_CASE("SteamVR is found from the config folder")
{
	namespace fs = std::filesystem;
	const fs::path base = fs::temp_directory_path() / "oc_entry_points_home";
	fs::remove_all(base);

	SUBCASE("XDG_CONFIG_HOME")
	{
		MakeConfig(base / "xdg", "/games/SteamVR");
		EnvGuard xdg("XDG_CONFIG_HOME", (base / "xdg").string().c_str());
		CHECK(BaseClientCore::GetAlternativeRuntimePath() == "/games/SteamVR");
	}

	SUBCASE("HOME with a trailing separator, a non-ASCII name")
	{
		const fs::path home = base / "h\xC3\xB6me";
		MakeConfig(home / ".config", "/games/SteamVR/");
		EnvGuard xdg("XDG_CONFIG_HOME", nullptr);
		EnvGuard h("HOME", (home.string() + "/").c_str());
		CHECK(BaseClientCore::GetAlternativeRuntimePath() == "/games/SteamVR");
	}

	SUBCASE("Nothing set")
	{
		EnvGuard xdg("XDG_CONFIG_HOME", nullptr);
		EnvGuard h("HOME", nullptr);
		CHECK(BaseClientCore::GetAlternativeRuntimePath().empty());
	}

	SUBCASE("An empty runtime entry")
	{
		MakeConfig(base / "xdg", "");
		EnvGuard xdg("XDG_CONFIG_HOME", (base / "xdg").string().c_str());
		CHECK(BaseClientCore::GetAlternativeRuntimePath().empty());
	}

	fs::remove_all(base);
}

#endif
