#include "stdafx.h"
#define BASE_IMPL
#include "BaseChaperone.h"
#include "BaseChaperoneSetup.h"
#include "BaseSystem.h"
#include "generated/static_bases.gen.h"

#include "Drivers/Backend.h"
#include "Misc/Bounds.h"
#include "Misc/ChaperoneData.h"
#include "Misc/SharedStore.h"

#include <algorithm>

#include <vector>

using namespace vr;

BaseChaperone::BaseChaperoneCalibrationState BaseChaperone::GetCalibrationState()
{
	// OK with a play area (the runtime's, else the committed room setup's), as the getters answer. Without one, a
	// warning rather than an error: games read an error as a room setup to do before they start, and a runtime
	// without stage bounds (Monado among them) still tracks standing
	const XrExtent2Df area = CurrentPlayArea();
	return area.width > 0 && area.height > 0 ? ChaperoneCalibrationState_OK : ChaperoneCalibrationState_Warning;
}

XrExtent2Df CurrentPlayArea()
{
	XrExtent2Df runtime{};
	bool known = false;
	if (BackendManager* backends = BackendManager::InstancePtr()) {
		int count = 0;
		if (backends->GetPlayAreaPoints(nullptr, &count) && count >= 2) {
			std::vector<vr::HmdVector3_t> points(count);
			if (backends->GetPlayAreaPoints(points.data(), nullptr)) {
				float minX = points[0].v[0], maxX = minX, minZ = points[0].v[2], maxZ = minZ;
				for (const vr::HmdVector3_t& p : points) {
					minX = std::min(minX, p.v[0]);
					maxX = std::max(maxX, p.v[0]);
					minZ = std::min(minZ, p.v[2]);
					maxZ = std::max(maxZ, p.v[2]);
				}
				runtime = { maxX - minX, maxZ - minZ };
				known = true;
			}
		}
	}
	return BoundsPlayArea(known ? &runtime : nullptr, known ? Json::Value() : SharedStore::Get().Read(StoreDoc::Chaperone));
}
bool BaseChaperone::GetPlayAreaSize(float* pSizeX, float* pSizeZ)
{
	if (!pSizeX || !pSizeZ)
		return false;
	*pSizeX = 0;
	*pSizeZ = 0;
	vr::HmdVector3_t minPoint, maxPoint;
	bool success = GetMinMaxPoints(minPoint, maxPoint);

	if (!success)
		return false;

	*pSizeX = maxPoint.v[0] - minPoint.v[0];
	*pSizeZ = maxPoint.v[2] - minPoint.v[2];

	return true;
}
void BaseChaperone::GetBoundsColor(HmdColor_t* pOutputColorArray, int nNumOutputColors)
{
	HmdColor_t camera;
	GetBoundsColor(pOutputColorArray, nNumOutputColors, 0.0f, &camera);
}

bool BaseChaperone::GetSoftBoundsInfo(OOVR_ChaperoneSoftBoundsInfo_t* pInfo)
{
	// Whether there are any, as SteamVR answers, with or without a struct to fill
	HmdQuad_t rect;
	return GetPlayAreaRect(pInfo ? &pInfo->quadCorners : &rect);
}

bool BaseChaperone::GetHardBoundsInfo(HmdQuad_t* pQuadsBuffer, uint32_t* punQuadsCount)
{
	return GetCreateBaseChaperoneSetup()->GetLiveCollisionBoundsInfo(pQuadsBuffer, punQuadsCount);
}

bool BaseChaperone::GetSeatedBoundsInfo(OOVR_ChaperoneSeatedBoundsInfo_t* pInfo)
{
	if (pInfo)
		*pInfo = {};
	return false;
}

bool BaseChaperone::GetPlayAreaRect(HmdQuad_t* rect)
{
	if (!rect)
		return false;
	memset(rect, 0, sizeof(vr::HmdQuad_t));

	vr::HmdVector3_t minPoint, maxPoint;
	bool success = GetMinMaxPoints(minPoint, maxPoint);

	if (!success)
		return false;

	// Counter-clockwise from the front right (the front being -Z)
	rect->vCorners[0].v[0] = maxPoint.v[0];
	rect->vCorners[0].v[2] = minPoint.v[2];

	rect->vCorners[1].v[0] = minPoint.v[0];
	rect->vCorners[1].v[2] = minPoint.v[2];

	rect->vCorners[2].v[0] = minPoint.v[0];
	rect->vCorners[2].v[2] = maxPoint.v[2];

	rect->vCorners[3].v[0] = maxPoint.v[0];
	rect->vCorners[3].v[2] = maxPoint.v[2];

	return true;
}
void BaseChaperone::ReloadInfo(void)
{
	// The play area is the OpenXR runtime's (asked once per session and after it changes), else the committed room
	// setup's, read from the store each time
}
void BaseChaperone::SetSceneColor(HmdColor_t color)
{
	OOVR_LOG_ONCE("No implementation");
}
void BaseChaperone::GetBoundsColor(HmdColor_t* pOutputColorArray, int nNumOutputColors, float flCollisionBoundsFadeDistance, HmdColor_t* pOutputCameraColor)
{
	// The colours of the committed room setup (IVRChaperoneSetup); before one is committed, clear bounds and a clear
	// white camera
	HmdColor_t bounds = ChaperoneData{}.colors[0], camera = ChaperoneData{}.colors[1];
	if (auto committed = ChaperoneData::FromJson(SharedStore::Get().Read(StoreDoc::Chaperone))) {
		bounds = committed->colors[0];
		camera = committed->colors[1];
	}
	// No more than any boundary has colours for: an app's count beyond that is a bad argument, not a buffer size
	if (pOutputColorArray && nNumOutputColors > 0)
		std::fill(pOutputColorArray, pOutputColorArray + std::min(nNumOutputColors, 1024), bounds);
	if (pOutputCameraColor)
		*pOutputCameraColor = camera;
}
bool BaseChaperone::AreBoundsVisible()
{
	return BackendManager::Instance().AreBoundsVisible();
}
void BaseChaperone::ForceBoundsVisible(bool bForce)
{
	return BackendManager::Instance().ForceBoundsVisible(bForce);
}

bool BaseChaperone::GetMinMaxPoints(vr::HmdVector3_t& minPoint, vr::HmdVector3_t& maxPoint)
{
	// The play area, centred on the standing origin
	const XrExtent2Df area = CurrentPlayArea();
	if (!(area.width > 0) || !(area.height > 0))
		return false; // no play area: the runtime has none and no room setup was committed
	minPoint = { -area.width / 2, 0, -area.height / 2 };
	maxPoint = { area.width / 2, 0, area.height / 2 };
	return true;
}

void BaseChaperone::ResetZeroPose(vr::ETrackingUniverseOrigin eTrackingUniverseOrigin)
{
	// The standing origin is the runtime's stage: moved under the head where the runtime allows it (Monado through
	// libmonado), which every program follows
	if (eTrackingUniverseOrigin == TrackingUniverseStanding) {
		TrackedDevicePose_t head{};
		if (std::shared_ptr<ITrackedDevice> hmd = BackendManager::Instance().GetDevice(vr::k_unTrackedDeviceIndex_Hmd))
			hmd->GetPose(vr::TrackingUniverseStanding, &head, ETrackingStateType::TrackingStateType_Rendering);
		if (head.bPoseIsValid && BaseChaperoneSetup::RecentreStage(head.mDeviceToAbsoluteTracking))
			return;
	}

	// Elsewhere the seated origin is all there is to reset
	if (BaseSystem* system = GetUnsafeBaseSystem())
		system->ResetSeatedZeroPose();
}
