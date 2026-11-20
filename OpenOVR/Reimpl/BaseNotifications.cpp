#include "stdafx.h"
#define BASE_IMPL
#include "BaseNotifications.h"
#include "BaseOverlay.h"
#include "generated/static_bases.gen.h"

#include <algorithm>

// EVRNotificationType_Persistent and _Transient_SystemWithUserValue
static constexpr OOVR_EVRNotificationType kTypePersistent = 1;
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
	vr::EVRNotificationError err = Add(type, ulUserValue, &id, ulOverlayHandle, pchText ? pchText : "");
	if (err != vr::VRNotificationError_OK)
		return err;
	if (pNotificationId)
		*pNotificationId = id;
	return vr::VRNotificationError_OK;
}

vr::EVRNotificationError BaseNotifications::Add(OOVR_EVRNotificationType type, uint64_t userValue, VRNotificationId* id,
    vr::VROverlayHandle_t overlay, const std::string& text)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (type == kTypeSystemWithUserValue) {
		for (const auto& n : live) {
			if (n.second.systemWithUserValue && n.second.userValue == userValue)
				return vr::VRNotificationError_SystemWithUserValueAlreadyExists;
		}
	}
	// A game that never removes its notifications doesn't grow the table without end: the oldest goes
	if (live.size() >= kMaxLive) {
		const uint32_t oldest = live.begin()->first;
		waiting.erase(std::remove(waiting.begin(), waiting.end(), oldest), waiting.end());
		if (shown == oldest)
			shown = 0;
		live.erase(live.begin());
	}
	// 0 is the invalid id, so skip it if the counter wraps
	uint32_t next = nextId++;
	if (next == 0)
		next = nextId++;
	live[next] = { type == kTypeSystemWithUserValue, type == kTypePersistent, userValue, overlay, text };
	waiting.push_back(next);
	*id = next;
	return vr::VRNotificationError_OK;
}

vr::EVRNotificationError BaseNotifications::RemoveNotification(OOVR_VRNotificationId notificationId)
{
	// SteamVR accepts the invalid id and refuses any other that isn't showing
	if (notificationId == 0)
		return vr::VRNotificationError_OK;

	std::lock_guard<std::mutex> lock(mutex);
	auto it = live.find(notificationId);
	if (it == live.end())
		return vr::VRNotificationError_InvalidNotificationId;
	if (shown == notificationId) {
		Queue(it->second, notificationId, vr::VREvent_Notification_Hidden);
		shown = 0;
	}
	Queue(it->second, notificationId, vr::VREvent_Notification_Destroyed);
	waiting.erase(std::remove(waiting.begin(), waiting.end(), notificationId), waiting.end());
	live.erase(it);
	return vr::VRNotificationError_OK;
}

void BaseNotifications::Queue(const Live& n, uint32_t id, uint32_t type)
{
	Event e{};
	e.overlay = n.overlay;
	e.event.eventType = type;
	e.event.trackedDeviceIndex = vr::k_unTrackedDeviceIndexInvalid;
	e.event.data.notification.ulUserValue = n.userValue;
	e.event.data.notification.notificationId = id;
	// A program without frames never takes them: the oldest go
	if (pending.size() >= kMaxLive)
		pending.erase(pending.begin());
	pending.push_back(e);
}

void BaseNotifications::Update(double now, std::vector<Event>& out)
{
	std::lock_guard<std::mutex> lock(mutex);
	// Its time over: hidden, and a transient one is gone
	if (shown && now - shownSince >= kShownSeconds) {
		auto it = live.find(shown);
		if (it != live.end()) {
			Queue(it->second, shown, vr::VREvent_Notification_Hidden);
			if (!it->second.persistent)
				live.erase(it);
		}
		shown = 0;
	}
	// The next one
	while (!shown && !waiting.empty()) {
		const uint32_t next = waiting.front();
		waiting.pop_front();
		auto it = live.find(next);
		if (it == live.end())
			continue;
		shown = next;
		shownSince = now;
		Queue(it->second, next, vr::VREvent_Notification_Shown);
	}
	out.insert(out.end(), pending.begin(), pending.end());
	pending.clear();
}

std::optional<BaseNotifications::Showing> BaseNotifications::Current()
{
	std::lock_guard<std::mutex> lock(mutex);
	auto it = live.find(shown);
	if (!shown || it == live.end())
		return std::nullopt;
	return Showing{ shown, it->second.text };
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
