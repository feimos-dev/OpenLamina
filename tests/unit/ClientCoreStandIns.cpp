// The test program links the client core (through the HMD and the other devices) but not the library that defines the
// OpenVR entry points the core calls, nor, on Windows, the module that holds the resources. These stand-ins let it
// link; no test reaches them.

#include "stdafx.h"

#include "steamvr_abi.h"

#ifdef _WIN32
// The module the resources are loaded from: the test program itself
HMODULE openovr_module_id = nullptr;
#endif

VR_INTERFACE void* VR_CALLTYPE VR_GetGenericInterface(const char*, vr::EVRInitError*) { return nullptr; }
VR_INTERFACE const char* VR_CALLTYPE VR_GetVRInitErrorAsEnglishDescription(vr::EVRInitError) { return ""; }
VR_INTERFACE const char* VR_CALLTYPE VR_GetVRInitErrorAsSymbol(vr::EVRInitError) { return ""; }
VR_INTERFACE uint32_t VR_CALLTYPE VR_InitInternal2(vr::EVRInitError*, vr::EVRApplicationType, const char*) { return 0; }
VR_INTERFACE bool VR_CALLTYPE VR_IsHmdPresent() { return false; }
VR_INTERFACE bool VR_CALLTYPE VR_IsInterfaceVersionValid(const char*) { return false; }
VR_INTERFACE void VR_CALLTYPE VR_ShutdownInternal() {}
