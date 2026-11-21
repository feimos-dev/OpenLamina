#pragma once
#include "../OpenOVR/Drivers/Backend.h"

#include <chrono>
#include <openxr/openxr.h>
#include <optional>
#include <string>

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
// The action manifest's path from VR_Init's startup info (JSON, from the game): nothing for text that isn't a JSON
// object with a string "action_manifest_path"
std::optional<std::string> ManifestPathFromStartupInfo(const char* startupInfo);
void GetXRAppName(char (&appName)[128]);
// Whether the OpenXR runtime has a headset (VR_IsHmdPresent). The instance that found one is kept, and the scene
// application's session that follows is started on it rather than on a second instance.
bool ProbeForHmd();
// Destroys a probe's instance kept for a session that won't come (a program that probed and never started)
void ReleaseProbedInstance();
// xrCreateInstance at the newest OpenXR version it takes (1.1, then 1.0, while it answers XR_ERROR_API_VERSION_UNSUPPORTED),
// through create; *version is the one it took
XrResult CreateInstanceNewestVersion(XrInstanceCreateInfo createInfo, XrInstance* instance, XrVersion* version,
    XrResult(XRAPI_PTR* create)(const XrInstanceCreateInfo*, XrInstance*));
// After the backend is made, on VR_Init's thread: a game whose lasers point at other programs' overlays (laserMouse),
// or which has a dashboard (dashboardButton) or the play area's walls (drawBounds), gets the overlays' instance now,
// even if it never asks for IVROverlay
void KeepOverlaysInstance();
// Stops OpenLamina's own frames for good, before VR_Shutdown destroys the interfaces their layers come from
void StopOwnFrames();
// A program exiting without VR_Shutdown (Linux's exit handler): ends the OpenXR instance, and with it the session, if
// the session lock is free within limit, and frees nothing else. False if the lock wasn't got: nothing was ended.
bool EndInstanceAtExit(std::chrono::milliseconds limit);
}; // namespace DrvOpenXR
