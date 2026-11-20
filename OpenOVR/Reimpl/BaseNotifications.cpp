#include "stdafx.h"
#define BASE_IMPL
#include "BaseNotifications.h"
#include "BaseOverlay.h"
#include "generated/static_bases.gen.h"

// EVRNotificationType_Transient_SystemWithUserValue
static constexpr OOVR_EVRNotificationType kTypeSystemWithUserValue = 2;

vr::EVRNotificationError BaseNotifications::CreateNotification(vr::VROverlayHandle_t ulOverlayHandle, uint64_t ulUserValue,
    OOVR_EVRNotificationType type, const char* pchText, OOVR_EVRNotificationStyle style,
    const OOVR_NotificationBitmap_t* pImage, VRNotificationId* pNotificationId)
{
	if (pNotificationId)
		*pNotificationId = 0;

	BaseOverlay* overlay = GetUnsafeBaseOverlay();
	if (!overlay || !overlay->_IsOwnOverlay(ulOverlayHandle))
		return vr::VRNotificationError_InvalidOverlayHandle;

	VRNotificationId id = 0;
	vr::EVRNotificationError err = Add(type, ulUserValue, &id);
	if (err != vr::VRNotificationError_OK)
		return err;

	OOVR_LOGF("Dropping notification: %s", pchText ? pchText : "(no text)");
	if (pNotificationId)
		*pNotificationId = id;
	return vr::VRNotificationError_OK;
}

vr::EVRNotificationError BaseNotifications::Add(OOVR_EVRNotificationType type, uint64_t userValue, VRNotificationId* id)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (type == kTypeSystemWithUserValue) {
		for (const auto& n : live) {
			if (n.second.systemWithUserValue && n.second.userValue == userValue)
				return vr::VRNotificationError_SystemWithUserValueAlreadyExists;
		}
	}
	// A game that never removes its notifications doesn't grow the table without end: the oldest goes
	if (live.size() >= kMaxLive)
		live.erase(live.begin());
	// 0 is the invalid id, so skip it if the counter wraps
	uint32_t next = nextId++;
	if (next == 0)
		next = nextId++;
	live[next] = { type == kTypeSystemWithUserValue, userValue };
	*id = next;
	return vr::VRNotificationError_OK;
}

vr::EVRNotificationError BaseNotifications::RemoveNotification(OOVR_VRNotificationId notificationId)
{
	// SteamVR accepts the invalid id and refuses any other that isn't showing
	if (notificationId == 0)
		return vr::VRNotificationError_OK;

	std::lock_guard<std::mutex> lock(mutex);
	return live.erase(notificationId) ? vr::VRNotificationError_OK : vr::VRNotificationError_InvalidNotificationId;
}

uint32_t BaseNotifications::GetErrorString(int error, char* pchBuffer, uint32_t unBufferSize)
{
	const char* name = error == 0 ? "k_ENotificationError_OK"
	    : error == 1             ? "k_ENotificationError_Fail"
	    : error == 2             ? "k_eNotificationError_InvalidParam"
	                             : "Unknown NotificationError_t";
	uint32_t needed = (uint32_t)strlen(name) + 1;
	if (pchBuffer && unBufferSize) {
		strncpy(pchBuffer, name, unBufferSize - 1);
		pchBuffer[unBufferSize - 1] = '\0';
	}
	return needed;
}
