//
// Created by ZNix on 25/10/2020.
//

#pragma once

#include "DeviceIdentity.h"
#include "XrDriverPrivate.h"

#include <string>

// HMD, and two controllers are reserved
constexpr int RESERVED_DEVICE_INDICES = 3;

class XrTrackedDevice : public virtual ITrackedDevice {
public:
	void GetPose(
	    vr::ETrackingUniverseOrigin origin,
	    vr::TrackedDevicePose_t* pose,
	    ETrackingStateType trackingState,
	    int64_t xrTime = 0) override;

	int32_t GetInt32TrackedDeviceProperty(vr::ETrackedDeviceProperty prop, vr::ETrackedPropertyError* pErrorL) override;
	uint64_t GetUint64TrackedDeviceProperty(vr::ETrackedDeviceProperty prop, vr::ETrackedPropertyError* pErrorL) override;
	uint32_t GetStringTrackedDeviceProperty(vr::ETrackedDeviceProperty prop, char* pchValue, uint32_t unBufferSize, vr::ETrackedPropertyError* pErrorL) override;

	/** What this device says it is (DeviceIdentity.h): each class gives its row */
	virtual DeviceIdentity Identity();

protected:
	/** The OpenXR time to locate the device at for trackingState, or xrTime where the caller names one; zero where no
	 * time is known. The caller holds the session lock. */
	static int64_t PoseTime(ETrackingStateType trackingState, int64_t xrTime);

	/** A string property's answer as SteamVR gives it: the size it needs with its terminator, copied only when it fits
	 * (else BufferTooSmall, and an empty string if there's room for one) */
	static uint32_t AnswerString(const std::string& text, char* value, uint32_t bufferSize, vr::ETrackedPropertyError* pErrorL);
};
