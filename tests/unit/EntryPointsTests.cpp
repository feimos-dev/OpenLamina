// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#ifndef _WIN32

#include "stdafx.h"

#include <cstring>
#include <dlfcn.h>
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

#endif
