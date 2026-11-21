#include "stdafx.h"
#define GENFILE
#include "BaseCommon.h"

GEN_INTERFACE("Notifications", "001")
GEN_INTERFACE("Notifications", "002")

#include "generated/GVRNotifications.gen.h"

// IVRNotifications_001 (SDK 0.9.x) named a notification's type and category with strings: dropped as today's are.
// SteamVR answers it with today's errors, not 0.9's
vr::IVRNotifications_001::NotificationError_t CVRNotifications_001::CreateNotification(vr::VROverlayHandle_t ulOverlayHandle,
    uint64_t ulUserValue, const char* strType, const char* strText, const char* strCategory,
    const vr::IVRNotifications_001::NotificationBitmap* photo, vr::IVRNotifications_001::VRNotificationId* notificationId)
{
	VRNotificationId id = 0;
	vr::EVRNotificationError err = base->CreateNotification(ulOverlayHandle, ulUserValue, 0, strText, 0, nullptr, &id);
	if (notificationId)
		*notificationId = id;
	return (vr::IVRNotifications_001::NotificationError_t)err;
}

vr::IVRNotifications_001::NotificationError_t CVRNotifications_001::DismissNotification(vr::IVRNotifications_001::VRNotificationId notificationId)
{
	return (vr::IVRNotifications_001::NotificationError_t)base->RemoveNotification(notificationId);
}

uint32_t CVRNotifications_001::GetErrorString(vr::IVRNotifications_001::NotificationError_t error, char* pchBuffer, uint32_t unBufferSize)
{
	// SteamVR names no error: an empty string, whatever the error
	if (pchBuffer && unBufferSize)
		pchBuffer[0] = '\0';
	return 1;
}
