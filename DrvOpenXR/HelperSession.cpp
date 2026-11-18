#include "HelperSession.h"

#include <atomic>

namespace {
std::atomic<SessionKind> currentKind{ SessionKind::Scene };

// Whether the runtime offers what this kind needs (XR_EXTX_overlay, XR_MND_headless); Scene: always
bool HelperSessionsAvailable(SessionKind kind, const std::set<std::string>& runtimeExtensions)
{
	switch (kind) {
	case SessionKind::Overlay:
		return runtimeExtensions.count(XR_EXTX_OVERLAY_EXTENSION_NAME) > 0;
	case SessionKind::Headless:
		return runtimeExtensions.count(XR_MND_HEADLESS_EXTENSION_NAME) > 0;
	case SessionKind::Scene:
	default:
		return true;
	}
}
} // namespace

SessionChoice ChooseSessionKind(vr::EVRApplicationType type, const std::set<std::string>& runtimeExtensions, bool helperApps, bool backgroundAsScene, bool programRunning)
{
	SessionChoice choice;
	if (type == vr::VRApplication_Scene) {
		choice.ok = true;
		return choice;
	}
	if (type == vr::VRApplication_Overlay) {
		if (helperApps && HelperSessionsAvailable(SessionKind::Overlay, runtimeExtensions)) {
			choice.ok = true;
			choice.kind = SessionKind::Overlay;
		} else {
			choice.refusal = vr::VRInitError_Init_InvalidApplicationType;
		}
		return choice;
	}
	if (type == vr::VRApplication_Background) {
		// Where background apps already get a scene session (Proton's helper relies on it), they keep it
		if (backgroundAsScene) {
			choice.ok = true;
		} else if (helperApps && programRunning && HelperSessionsAvailable(SessionKind::Headless, runtimeExtensions)) {
			choice.ok = true;
			choice.kind = SessionKind::Headless;
		} else {
			choice.refusal = vr::VRInitError_Init_NoServerForBackgroundApp;
		}
		return choice;
	}
	choice.refusal = vr::VRInitError_Init_InvalidApplicationType;
	return choice;
}

bool BackgroundAppsGetSceneSessions()
{
#ifdef _WIN32
	return false;
#else
	return true;
#endif
}

vr::EVRInitError RefusalWithoutSession(vr::EVRApplicationType type, bool helperApps, vr::EVRInitError runtimeError)
{
	const SessionChoice choice = ChooseSessionKind(type, {}, helperApps, BackgroundAppsGetSceneSessions(), true);
	return choice.ok ? runtimeError : choice.refusal;
}

SessionKind CurrentSessionKind()
{
	return currentKind;
}

void SetCurrentSessionKind(SessionKind kind)
{
	currentKind = kind;
}

const void* HelperSessionChain::Chain(SessionKind kind, const void* binding, uint32_t layersPlacement)
{
	if (kind != SessionKind::Overlay)
		return binding; // a headless session is one without a binding
	overlay = { XR_TYPE_SESSION_CREATE_INFO_OVERLAY_EXTX };
	overlay.next = binding;
	overlay.createFlags = 0;
	overlay.sessionLayersPlacement = layersPlacement;
	return &overlay;
}
