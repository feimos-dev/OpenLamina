#pragma once
#include "BaseCommon.h"

#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// The generated wrapper passes these as the OOVR_ types, and its out parameter as VRNotificationId.
typedef int OOVR_EVRNotificationType;
typedef int OOVR_EVRNotificationStyle;
typedef void OOVR_NotificationBitmap_t;
typedef uint32_t OOVR_VRNotificationId;
typedef uint32_t VRNotificationId;

// SteamVR's notifications: shown one at a time, in the order they came, each for a few seconds as a panel ahead of the
// head (BaseOverlay draws it), with VREvent_Notification_Shown, _Hidden and _Destroyed for the overlay they're on. A
// transient one goes once it's been shown; a persistent one stays until it's removed, shown once.
class BaseNotifications {
public:
	vr::EVRNotificationError CreateNotification(vr::VROverlayHandle_t ulOverlayHandle, uint64_t ulUserValue,
	    OOVR_EVRNotificationType type, const char* pchText, OOVR_EVRNotificationStyle style,
	    const OOVR_NotificationBitmap_t* pImage, VRNotificationId* pNotificationId);

	vr::EVRNotificationError RemoveNotification(OOVR_VRNotificationId notificationId);

	// Records a notification as live and gives its id; the part of CreateNotification after the overlay check
	vr::EVRNotificationError Add(OOVR_EVRNotificationType type, uint64_t userValue, VRNotificationId* id,
	    vr::VROverlayHandle_t overlay = vr::k_ulOverlayHandleInvalid, const std::string& text = std::string());

	// An event for the overlay a notification is on
	struct Event {
		vr::VROverlayHandle_t overlay;
		vr::VREvent_t event; // its data's notification part filled in
	};
	// Moves the notifications on to now (seconds, any steady clock): the events since the last call
	void Update(double now, std::vector<Event>& out);
	// The notification shown now
	struct Showing {
		uint32_t id;
		std::string text;
	};
	std::optional<Showing> Current();

	static constexpr double kShownSeconds = 5.0;

private:
	struct Live {
		bool systemWithUserValue;
		bool persistent;
		uint64_t userValue;
		vr::VROverlayHandle_t overlay;
		std::string text;
	};
	void Queue(const Live& n, uint32_t id, uint32_t type);

	std::mutex mutex;
	uint32_t nextId = 1;
	static constexpr size_t kMaxLive = 256;
	std::map<uint32_t, Live> live;
	std::deque<uint32_t> waiting; // to be shown, in order
	uint32_t shown = 0; // shown now, 0 for none
	double shownSince = 0;
	std::vector<Event> pending; // events from calls, given out at the next Update
};
