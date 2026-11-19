#pragma once
#include "BaseCommon.h"

#include <map>
#include <mutex>

// The generated wrapper passes these as the OOVR_ types, and its out parameter as VRNotificationId.
typedef int OOVR_EVRNotificationType;
typedef int OOVR_EVRNotificationStyle;
typedef void OOVR_NotificationBitmap_t;
typedef uint32_t OOVR_VRNotificationId;
typedef uint32_t VRNotificationId;

// SteamVR's desktop-style notifications, which there's nowhere to show: they're kept as ids and
// not drawn, so apps that ask for the interface (Batman: Arkham VR) carry on.
class BaseNotifications {
public:
	vr::EVRNotificationError CreateNotification(vr::VROverlayHandle_t ulOverlayHandle, uint64_t ulUserValue,
	    OOVR_EVRNotificationType type, const char* pchText, OOVR_EVRNotificationStyle style,
	    const OOVR_NotificationBitmap_t* pImage, VRNotificationId* pNotificationId);

	vr::EVRNotificationError RemoveNotification(OOVR_VRNotificationId notificationId);

	// Records a notification as live and gives its id; the part of CreateNotification after the overlay check
	vr::EVRNotificationError Add(OOVR_EVRNotificationType type, uint64_t userValue, VRNotificationId* id);

	/** IVRNotifications_001 (SDK 0.9.x): its error names (OK, Fail, InvalidParam), as SteamVR spelt them */
	uint32_t GetErrorString(int error, VR_OUT_STRING() char* pchBuffer, uint32_t unBufferSize);

private:
	struct Live {
		bool systemWithUserValue;
		uint64_t userValue;
	};

	std::mutex mutex;
	uint32_t nextId = 1;
	std::map<uint32_t, Live> live;
};
