#pragma once
#include "../OpenOVR/Drivers/Backend.h"

// Since we're not importing stdafx, add another nasty little hack
#ifndef _WIN32
#include "../OpenOVR/linux_funcs.h"
#endif

namespace DrvOpenXR {
// nullptr with *error set if OpenXR can't be started (108 HmdNotFound for no runtime or no headset, else 124
// Internal), with everything it had created destroyed again. An Overlay or Background appType gets an overlay or a
// headless session where the runtime has them (helperApps), and is otherwise refused (130 InvalidApplicationType, 121
// NoServerForBackgroundApp), also when the runtime fails to start; on Linux a Background app gets a scene session.
IBackend* CreateOpenXRBackend(const char* startupInfo, vr::EVRInitError* error, vr::EVRApplicationType appType = vr::VRApplication_Scene);
void GetXRAppName(char (&appName)[128]);
// Stops OpenLamina's own frames for good, before VR_Shutdown destroys the interfaces their layers come from
void StopOwnFrames();
}; // namespace DrvOpenXR
