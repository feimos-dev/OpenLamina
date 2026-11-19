#include "doctest.h"

#include "stdafx.h"

#include "XrHMD.h"

#ifndef _WIN32
#include "steamvr_abi.h"

// Linking the HMD pulls in the client core, which calls the OpenVR entry points that only the vrclient library
// itself defines. Nothing here reaches them.
VR_INTERFACE void* VR_CALLTYPE VR_GetGenericInterface(const char*, vr::EVRInitError*) { return nullptr; }
VR_INTERFACE const char* VR_CALLTYPE VR_GetVRInitErrorAsEnglishDescription(vr::EVRInitError) { return ""; }
VR_INTERFACE const char* VR_CALLTYPE VR_GetVRInitErrorAsSymbol(vr::EVRInitError) { return ""; }
VR_INTERFACE uint32_t VR_CALLTYPE VR_InitInternal2(vr::EVRInitError*, vr::EVRApplicationType, const char*) { return 0; }
VR_INTERFACE bool VR_CALLTYPE VR_IsHmdPresent() { return false; }
VR_INTERFACE bool VR_CALLTYPE VR_IsInterfaceVersionValid(const char*) { return false; }
VR_INTERFACE void VR_CALLTYPE VR_ShutdownInternal() {}
#endif

TEST_CASE("The IPD before the runtime has views is a human one")
{
	// Without a session there are no views to measure
	XrHMD hmd;
	CHECK(hmd.GetIPD() == doctest::Approx(0.064f));
}
