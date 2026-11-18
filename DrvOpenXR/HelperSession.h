#pragma once
// What kind of OpenXR session a program gets: a game's scene session, or a helper program's (an overlay application's
// overlay session, XR_EXTX_overlay; a background application's headless one, XR_MND_headless).

#include "generated/interfaces/vrtypes.h"

#include <openxr/openxr.h>

#include <set>
#include <string>

enum class SessionKind { Scene, Overlay, Headless };

// What an application type gets: a session of a kind, or (ok false) the init error it is refused with
struct SessionChoice {
	bool ok = false;
	SessionKind kind = SessionKind::Scene;
	vr::EVRInitError refusal = vr::VRInitError_None;
};
// Overlay and Background apps get helper sessions where the runtime has them and helperApps is on, and are otherwise
// refused (130 InvalidApplicationType, 121 NoServerForBackgroundApp). A Background app also needs programRunning
// (another OpenLamina program is running): it doesn't start the runtime, as SteamVR doesn't start for it. With
// backgroundAsScene (Linux, where Proton's helper relies on it), a Background app gets a scene session.
SessionChoice ChooseSessionKind(vr::EVRApplicationType type, const std::set<std::string>& runtimeExtensions, bool helperApps, bool backgroundAsScene, bool programRunning);

// Whether Background apps get scene sessions here (Linux)
bool BackgroundAppsGetSceneSessions();

// The refusal an application type gets where its session can't be had (no runtime, or the runtime fails to start):
// 130 for Overlay and 121 for Background apps that would get a helper session, else runtimeError
vr::EVRInitError RefusalWithoutSession(vr::EVRApplicationType type, bool helperApps, vr::EVRInitError runtimeError);

// The current backend's kind (Scene without one)
SessionKind CurrentSessionKind();
void SetCurrentSessionKind(SessionKind kind);
inline bool IsHelperSession() { return CurrentSessionKind() != SessionKind::Scene; }

// The create info chain for the kind's session, in front of binding (the graphics binding, nullptr for headless).
// Points into this object, which must outlive xrCreateSession.
class HelperSessionChain {
public:
	const void* Chain(SessionKind kind, const void* binding, uint32_t layersPlacement);

private:
	XrSessionCreateInfoOverlayEXTX overlay{ XR_TYPE_SESSION_CREATE_INFO_OVERLAY_EXTX };
};
