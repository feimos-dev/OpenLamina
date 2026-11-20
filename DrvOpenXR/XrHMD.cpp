//
// Created by ZNix on 25/10/2020.
//

#include "XrHMD.h"
#include "XrBackend.h"

#include <atomic>

#include "../OpenOVR/Misc/Config.h"
#include "../OpenOVR/Misc/LibMonado.h"
#include "../OpenOVR/Misc/xr_ext.h"
#include "../OpenOVR/Misc/xrmoreutils.h"
#include "../OpenOVR/Reimpl/BaseSystem.h"
#include "../OpenOVR/Reimpl/BaseSettings.h"
#include "../OpenOVR/Misc/SharedStore.h"
#include "../OpenOVR/convert.h"
#include "generated/static_bases.gen.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <mutex>
#include <vector>

// The session globals are briefly null while another thread restarts the session: wait up to 2 s for them. False if
// there's still no session (it was lost, or a restart failed), so the caller answers without one.
static bool WaitForSessionGlobals()
{
	// A lost session doesn't come back: no waiting for it
	auto IsLost = []() {
		BackendManager* manager = BackendManager::InstancePtr();
		auto* backend = manager ? (XrBackend*)manager->GetBackendInstance() : nullptr;
		return backend && backend->IsSessionLost();
	};
	for (int i = 0; !xr_gbl && !IsLost() && i < 100; i++) {
		using namespace std::chrono_literals;
		std::this_thread::sleep_for(20ms);
	}
	return xr_gbl != nullptr;
}

static std::atomic<float> answeredRenderScale{ 0.0f };

float XrHMD::AnsweredRenderScale()
{
	return answeredRenderScale;
}

uint32_t XrHMD::ScaledImageSize(uint32_t recommended, uint32_t maximum, float scale)
{
	if (!(scale > 0.0f))
		scale = 1.0f;
	double scaled = std::round((double)recommended * (double)scale);
	if (maximum > 0 && scaled > (double)maximum)
		scaled = (double)maximum;
	// Never zero for a view that has a size
	return recommended > 0 && scaled < 1.0 ? 1 : (uint32_t)scaled;
}

void XrHMD::GetRecommendedRenderTargetSize(uint32_t* width, uint32_t* height)
{
	// openlamina.ini's supersampleRatio, unless a program or the user's SteamVR settings chose another
	float scale = BaseSettings::RenderScale(SharedStore::Get(), oovr_global_configuration.SupersampleRatio());
	answeredRenderScale = scale;

	// One size for both eyes: what the larger of the two needs, within what the smaller can have
	const XrViewConfigurationView& left = xr_main_view(XruEyeLeft);
	const XrViewConfigurationView& right = xr_main_view(XruEyeRight);
	auto smaller = [](uint32_t a, uint32_t b) { return a && b ? std::min(a, b) : std::max(a, b); };
	if (width)
		*width = ScaledImageSize(std::max(left.recommendedImageRectWidth, right.recommendedImageRectWidth),
		    smaller(left.maxImageRectWidth, right.maxImageRectWidth), scale);
	if (height)
		*height = ScaledImageSize(std::max(left.recommendedImageRectHeight, right.recommendedImageRectHeight),
		    smaller(left.maxImageRectHeight, right.maxImageRectHeight), scale);
}

// from BaseSystem

bool XrHMD::IsValidFov(const XrFovf& fov)
{
	// Each angle short of a right angle, whose tangent is a number, and a view that has some size
	const float limit = 1.5707f;
	for (float angle : { fov.angleLeft, fov.angleRight, fov.angleUp, fov.angleDown }) {
		if (!std::isfinite(angle) || std::fabs(angle) >= limit)
			return false;
	}
	return fov.angleLeft < fov.angleRight && fov.angleDown < fov.angleUp;
}

// The eye's field of view from the runtime's views. Where those aren't valid (before the first frame, a runtime that
// reports zeros early on) the last valid one stays, and before any a 90 degree view.
static XrFovf CurrentFov(vr::EVREye eye)
{
	static std::mutex fovMutex;
	const float quarterTurn = 0.7853982f; // 45 degrees, whose tangent is 1
	static XrFovf lastValid[XruEyeCount] = { { -quarterTurn, quarterTurn, quarterTurn, -quarterTurn },
		{ -quarterTurn, quarterTurn, quarterTurn, -quarterTurn } };

	std::lock_guard<std::mutex> lock(fovMutex);
	if (xr_gbl) {
		const XruCachedViews cachedViews = xr_gbl->GetCachedViews(xr_gbl->seatedSpace);
		if (cachedViews.viewCount == XruEyeCount) {
			for (int i = 0; i < XruEyeCount; i++) {
				if (XrHMD::IsValidFov(cachedViews.views[i].fov))
					lastValid[i] = cachedViews.views[i].fov;
			}
		}
	}
	return lastValid[eye];
}

vr::HmdMatrix44_t XrHMD::ProjectionFromTangents(float tanL, float tanR, float tanU, float tanD, float fNearZ, float fFarZ, EGraphicsAPIConvention convention)
{
	float horizontalFov = -tanL + tanR;
	float verticalFov = tanU - tanD;

	// To make building these projections easier, we build them as row-major matrices then (for graphics
	// APIs that use column-major matrices) transpose them at the end if needed.
	// It looks like there aren't any functions in glm that can take different l/r/t/b FOV values, so do it ourselves
	glm::mat4 m;
	if (convention == API_DirectX) {
		// Depth from 0 at the near plane to 1 at the far plane, like glm::perspectiveLH_ZO
		m[0] = glm::vec4(2 / horizontalFov, 0, (tanL + tanR) / horizontalFov, 0);
		m[1] = glm::vec4(0, 2 / verticalFov, (tanU + tanD) / verticalFov, 0);
		m[2] = glm::vec4(0, 0, -fFarZ / (fFarZ - fNearZ), -(fFarZ * fNearZ) / (fFarZ - fNearZ));
		m[3] = glm::vec4(0, 0, -1, 0);
	} else {
		// The same off-axis frustum, with OpenGL's -1..1 depth range: glFrustum with l, r, t, b = near * tan(angle)
		m[0] = glm::vec4(2 / horizontalFov, 0, (tanL + tanR) / horizontalFov, 0); // First row, determines X
		m[1] = glm::vec4(0, 2 / verticalFov, (tanU + tanD) / verticalFov, 0); // Determines Y
		m[2] = glm::vec4(0, 0, -(fNearZ + fFarZ) / (fFarZ - fNearZ), -(2 * fNearZ * fFarZ) / (fFarZ - fNearZ)); // Determines Z
		m[3] = glm::vec4(0, 0, -1, 0); // Determines W
	}

	// OpenVR's matrices are row-major for every API
	return O2S_m4(m);
}

vr::HmdMatrix44_t XrHMD::GetProjectionMatrix(vr::EVREye eEye, float fNearZ, float fFarZ, EGraphicsAPIConvention convention)
{
	auto sessionLock = xr_session.lock_shared(); // held while reading: a session restart waits for it
	if (eEye < 0 || (int)eEye >= 2)
		eEye = vr::Eye_Left;

	const XrFovf fov = CurrentFov(eEye);
	return ProjectionFromTangents(tanf(fov.angleLeft), tanf(fov.angleRight), tanf(fov.angleUp), tanf(fov.angleDown), fNearZ, fFarZ, convention);
}

void XrHMD::GetProjectionRaw(vr::EVREye eEye, float* pfLeft, float* pfRight, float* pfTop, float* pfBottom)
{
	auto sessionLock = xr_session.lock_shared(); // held while reading: a session restart waits for it
	// This is how SteamVR seems to handle invalid eyes
	if (eEye < 0 || (int)eEye >= 2)
		eEye = vr::Eye_Left;

	const XrFovf fov = CurrentFov(eEye);

	/**
	 * With a straight passthrough:
	 *
	 * SteamVR Left:  -1.110925, 0.889498, -0.964926, 0.715264
	 * SteamVR Right: -1.110925, 0.889498, -0.715264, 0.964926
	 *
	 * For the Rift S:
	 * OpenXR Left: 1.000000, -1.150368, -0.965689, 1.035530
	 * SteamVR Left: -1.150368, 1.000000, -0.965689, 1.035530
	 *
	 * Via:
	 *   char buff[1024];
	 *   snprintf(buff, sizeof(buff), "eye=%d %f, %f, %f, %f", eye, *pfTop, *pfBottom, *pfLeft, *pfRight);
	 *   OOVR_LOG(buff);
	 *
	 * This suggests that SteamVR negates the top and left values, which obviously we need to match. OpenXR
	 * also negates the bottom and left value. Since it appears that SteamVR flips the top and bottom angles, we
	 * can just do that and it'll match.
	 */

	if (pfTop)
		*pfTop = tanf(fov.angleDown);
	if (pfBottom)
		*pfBottom = tanf(fov.angleUp);
	if (pfLeft)
		*pfLeft = tanf(fov.angleLeft);
	if (pfRight)
		*pfRight = tanf(fov.angleRight);
}

bool XrHMD::ComputeDistortion(vr::EVREye eEye, float fU, float fV, vr::DistortionCoordinates_t* pDistortionCoordinates)
{
	// The OpenXR runtime distorts the frames itself and doesn't say how: the coordinates come back as given
	if (!pDistortionCoordinates || (eEye != vr::Eye_Left && eEye != vr::Eye_Right))
		return false;
	pDistortionCoordinates->rfRed[0] = pDistortionCoordinates->rfGreen[0] = pDistortionCoordinates->rfBlue[0] = fU;
	pDistortionCoordinates->rfRed[1] = pDistortionCoordinates->rfGreen[1] = pDistortionCoordinates->rfBlue[1] = fV;
	return true;
}

vr::HmdMatrix34_t XrHMD::GetEyeToHeadTransform(vr::EVREye eEye)
{
	auto sessionLock = xr_session.lock_shared(); // held while reading: a session restart waits for it
	// The views from the last frame that had both, shared by every thread asking
	static std::mutex viewsMutex;
	static XrTime time = ~0; // Don't set to zero by default, otherwise we'll return an identity matrix before the first frame
	static XrView views[XruEyeCount] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };

	if (!WaitForSessionGlobals())
		return G2S_m34(glm::identity<glm::mat4>()); // no session: the eyes at the head

	// This won't exactly work for HMDs designed for spiders, but it's how SteamVR handles invalid eye numbers.
	if (eEye < 0 || (int)eEye >= 2)
		eEye = vr::Eye_Left;

	std::lock_guard<std::mutex> viewsLock(viewsMutex);
	if (time != xr_gbl->GetBestTime()) {
		const XruCachedViews& cachedViews = xr_gbl->GetCachedViews(xr_gbl->viewSpace);
		const XrViewState& viewState = cachedViews.viewState;
		// Without both views the last ones stay
		if (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT && viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT
		    && cachedViews.viewCount == XruEyeCount) {
			views[0] = cachedViews.views[0];
			views[1] = cachedViews.views[1];
			time = xr_gbl->GetBestTime();
		}
	}

	return G2S_m34(X2G_om34_pose(views[eEye].pose));
}

bool XrHMD::VsyncPhase(XrTime now, XrTime anchor, XrDuration period, float* secondsSinceVsync, uint64_t* vsyncCount)
{
	if (secondsSinceVsync)
		*secondsSinceVsync = 0.0f;
	if (vsyncCount)
		*vsyncCount = 0;
	if (now <= 0 || anchor <= 1 || period <= 0)
		return false;

	// The display's vsyncs are at anchor + k * period, for whole k (negative for those before the anchor)
	XrDuration offset = now - anchor;
	XrDuration whole = offset / period;
	XrDuration rest = offset % period;
	if (rest < 0) {
		rest += period;
		whole--;
	}
	XrTime count = anchor / period + whole;
	if (count < 0)
		return false;

	if (secondsSinceVsync)
		*secondsSinceVsync = (float)((double)rest / 1e9);
	if (vsyncCount)
		*vsyncCount = (uint64_t)count;
	return true;
}

bool XrHMD::GetTimeSinceLastVsync(float* pfSecondsSinceLastVsync, uint64_t* pulFrameCounter)
{
	if (pfSecondsSinceLastVsync)
		*pfSecondsSinceLastVsync = 0.0f;
	if (pulFrameCounter)
		*pulFrameCounter = 0;

	auto lock = xr_session.lock_shared(); // held while reading: a session restart waits for it
	if (!xr_gbl)
		return false;

	// The runtime's last predicted display time is on the display's vsync grid; the period is the runtime's
	BackendManager* manager = BackendManager::InstancePtr();
	auto* backend = manager ? (XrBackend*)manager->GetBackendInstance() : nullptr;
	float periodMs = backend ? backend->DisplayPeriodMs() : 0.0f;
	return VsyncPhase(xr_time_now(), xr_gbl->nextPredictedFrameTime, (XrDuration)(periodMs * 1e6f), pfSecondsSinceLastVsync, pulFrameCounter);
}

vr::HiddenAreaMesh_t XrHMD::GetHiddenAreaMesh(vr::EVREye eEye, vr::EHiddenAreaMeshType type)
{
	if (!xr_ext->xrGetVisibilityMaskKHR_Available()) {
		// This is what the docs say we should return if the mask is unavailable
		return HiddenAreaMesh(nullptr, 0);
	}

	// TODO verify the line loop mode works properly

	XrVisibilityMaskTypeKHR xrType;
	switch (type) {
	case vr::k_eHiddenAreaMesh_Standard:
		xrType = XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR;
		break;
	case vr::k_eHiddenAreaMesh_Inverse:
		xrType = XR_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH_KHR;
		break;
	case vr::k_eHiddenAreaMesh_LineLoop:
		xrType = XR_VISIBILITY_MASK_TYPE_LINE_LOOP_KHR;
		break;
	default:
		// As SteamVR answers a mesh type it doesn't know: no mesh
		return HiddenAreaMesh(nullptr, 0);
	}

	// Note: the OpenXR and OpenVR eye indexes are the same, so we can just cast between them.
	auto eye = (uint32_t)eEye;

	// First find out what size we need to allocate
	// Note that this doesn't return XR_ERROR_SIZE_INSUFFICIENT, since setting one of the input counts to zero is special-cased
	XrVisibilityMaskKHR mask = { XR_TYPE_VISIBILITY_MASK_KHR };
	if (eye >= XruEyeCount || !xr_session.get() || XR_FAILED(xr_ext->xrGetVisibilityMaskKHR(xr_session.get(), XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, eye, xrType, &mask))
	    || mask.indexCountOutput == 0) {
		// No mask from the runtime: what the docs say to return when there's none
		return HiddenAreaMesh(nullptr, 0);
	}

	// Allocate memory for the mesh
	std::vector<uint32_t> indices(mask.indexCountOutput);
	std::vector<XrVector2f> vertices(mask.vertexCountOutput);
	mask.vertexCapacityInput = (uint32_t)vertices.size();
	mask.indexCapacityInput = (uint32_t)indices.size();
	mask.indices = indices.data();
	mask.vertices = vertices.data();

	// Now actually request the mask data
	if (XR_FAILED(xr_ext->xrGetVisibilityMaskKHR(xr_session.get(), XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, eye, xrType, &mask))
	    || mask.indexCountOutput > indices.size() || mask.vertexCountOutput > vertices.size()) {
		return HiddenAreaMesh(nullptr, 0);
	}

	// Convert the data into something usable by SteamVR - it doesn't use indices
	std::vector<vr::HmdVector2_t> mesh(mask.indexCountOutput);

	float ftop, fbottom, fleft, fright;
	GetProjectionRaw(eEye, &fleft, &fright, &ftop, &fbottom);

	for (uint32_t i = 0; i < mask.indexCountOutput; i++) {
		if (indices[i] >= mask.vertexCountOutput)
			return HiddenAreaMesh(nullptr, 0);
		XrVector2f v = vertices[indices[i]];

		if (oovr_global_configuration.EnableHiddenMeshFix()) {
			if (fabs(v.y - ftop) > 0.001 && fabs(v.y - fbottom) > 0.001) {
				mesh[i] = vr::HmdVector2_t{ (v.x - fleft) / (fright - fleft), (v.y * oovr_global_configuration.HiddenMeshVerticalScale() - ftop) / (fbottom - ftop) };
			} else {
				mesh[i] = vr::HmdVector2_t{ (v.x - fleft) / (fright - fleft), (v.y - ftop) / (fbottom - ftop) };
			}
		} else {
			mesh[i] = vr::HmdVector2_t{ v.x, v.y };
		}
	}

	uint32_t count = type == vr::k_eHiddenAreaMesh_LineLoop ? mask.indexCountOutput : mask.indexCountOutput / 3;

	// The caller reads the vertices after this returns, with no call to say it's done: the HMD keeps each eye's and
	// type's mesh, and a mesh that comes out the same as the last one stays where it is. The one before is kept too,
	// for a caller still reading it when the mesh changes.
	std::lock_guard<std::mutex> lock(hiddenMeshMutex);
	HiddenMesh& kept = hiddenMeshes[eye][(int)type];
	bool same = kept.current.size() == mesh.size() && std::memcmp(kept.current.data(), mesh.data(), mesh.size() * sizeof(vr::HmdVector2_t)) == 0;
	if (!same) {
		kept.previous = std::move(kept.current);
		kept.current = std::move(mesh);
	}
	return HiddenAreaMesh(kept.current.data(), count);
}

void XrHMD::GetPose(vr::ETrackingUniverseOrigin origin, vr::TrackedDevicePose_t* pose, ETrackingStateType trackingState, int64_t xrTime)
{
	// Held to the end: a session restart on another thread replaces xr_gbl and its spaces, and waits for this to end
	auto lock = xr_session.lock_shared();
	if (!xr_gbl) {
		// No session (it was lost, or a restart failed): no pose
		if (pose) {
			*pose = {};
			pose->eTrackingResult = vr::TrackingResult_Uninitialized;
		}
		return;
	}
	if (!pose)
		return;

	// What stays when the runtime doesn't track the head: connected, but out of range
	*pose = {};
	pose->bDeviceIsConnected = true;
	pose->eTrackingResult = vr::TrackingResult_Running_OutOfRange;
	xr_utils::PoseFromSpace(pose, xr_gbl->viewSpace, origin, {}, PoseTime(trackingState, xrTime));
}

std::optional<float> XrHMD::ViewsIpd()
{
	auto lock = xr_session.lock_shared(); // held while reading: a session restart waits for it
	if (!xr_gbl)
		return {};
	const XruCachedViews cachedViews = xr_gbl->GetCachedViews(xr_gbl->viewSpace);
	const XrViewState& state = cachedViews.viewState;
	if (cachedViews.viewCount != XruEyeCount || !(state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) || !(state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT))
		return {};
	return cachedViews.views[vr::Eye_Right].pose.position.x - cachedViews.views[vr::Eye_Left].pose.position.x;
}

// The average adult IPD, 64 mm, until the runtime's views say otherwise
static const float kDefaultIpd = 0.064f;

float XrHMD::GetIPD()
{
	if (!xr_gbl)
		return kDefaultIpd;

	static float ipd = kDefaultIpd;

	// Without both views the last IPD stays
	if (std::optional<float> views = ViewsIpd())
		ipd = *views;

	return ipd;
}

static std::atomic<bool> userPresent{ true };

bool XrHMD::SetUserPresent(bool present)
{
	return userPresent.exchange(present) != present;
}

bool XrHMD::HasProximitySensor()
{
	auto lock = xr_session.lock_shared(); // held while reading: a session restart waits for it
	return xr_gbl && xr_gbl->supportsUserPresence;
}

bool XrHMD::GetControllerState(vr::VRControllerState_t* state)
{
	*state = {};
	if (!HasProximitySensor())
		return false;
	if (userPresent)
		state->ulButtonPressed = 1ull << vr::k_EButton_ProximitySensor;
	return true;
}

#define TRY_PROFILE_PROP(type)                                                     \
	do {                                                                           \
		std::shared_lock profileLock(profile_mutex);                               \
		if (profile) {                                                             \
			std::optional<type> ret = profile->GetProperty<type>(prop, HAND_NONE); \
			if (ret.has_value())                                                   \
				return *ret;                                                       \
		}                                                                          \
	} while (0)

// Properties
bool XrHMD::GetBoolTrackedDeviceProperty(vr::ETrackedDeviceProperty prop, vr::ETrackedPropertyError* pErrorL)
{
	if (pErrorL)
		*pErrorL = vr::TrackedProp_Success;

	TRY_PROFILE_PROP(bool);

	switch (prop) {
	case vr::Prop_DeviceProvidesBatteryStatus_Bool: {
		// From libmonado when Monado offers it
		std::optional<LibMonado::Battery> battery = LibMonado::Get().BatteryForRole("head");
		return battery && battery->present;
	}
	case vr::Prop_DeviceIsCharging_Bool: {
		std::optional<LibMonado::Battery> battery = LibMonado::Get().BatteryForRole("head");
		if (battery && battery->present)
			return battery->charging;
		break;
	}
	case vr::Prop_ReportsTimeSinceVSync_Bool: {
		// GetTimeSinceLastVsync needs the runtime's clock
		auto lock = xr_session.lock_shared();
		return xr_gbl && xr_time_now() > 0;
	}
	case vr::Prop_HasDriverDirectModeComponent_Bool:
		return true; // Who knows what this is used for? It's in HL:A anyway.
	case vr::Prop_ContainsProximitySensor_Bool:
		// With sessionEvents, the runtime says whether the user wears the headset
		return oovr_global_configuration.SessionEvents() ? HasProximitySensor() : true;
	case vr::Prop_HasCameraComponent_Bool:
	case vr::Prop_HasCamera_Bool:
	case vr::Prop_IsOnDesktop_Bool:
		return false;
	case vr::Prop_NeverTracked_Bool:
	case vr::Prop_WillDriftInYaw_Bool:
	case vr::Prop_DeviceHasNoIMU_Bool: {
		// From what the system says it tracks: a system with no tracking never has a pose; one with orientation only
		// has no position to hold its yaw by; one without orientation has no IMU
		auto lock = xr_session.lock_shared();
		if (!xr_gbl)
			break;
		const XrSystemTrackingProperties& tracking = xr_gbl->systemProperties.trackingProperties;
		bool orientation = tracking.orientationTracking != XR_FALSE, position = tracking.positionTracking != XR_FALSE;
		if (prop == vr::Prop_NeverTracked_Bool)
			return !orientation && !position;
		if (prop == vr::Prop_WillDriftInYaw_Bool)
			return orientation && !position;
		return !orientation;
	}
	case vr::Prop_SupportsXrEyeGazeInteraction_Bool: {
		auto lock = xr_session.lock_shared();
		if (!xr_gbl)
			break;
		return xr_gbl->eyeGazeProperties.supportsEyeGazeInteraction != XR_FALSE;
	}
	case vr::Prop_HasDisplayComponent_Bool:
		return true;
	case vr::Prop_HasVirtualDisplayComponent_Bool:
		return false;
	default:
		break;
	}

	return XrTrackedDevice::GetBoolTrackedDeviceProperty(prop, pErrorL);
}

// The runtime's refresh rate where it says (XR_FB_display_refresh_rate), else the measured display period's, else 90
static float DisplayFrequency()
{
	if (xr_ext && xr_ext->DisplayRefreshRateFB_Available()) {
		auto lock = xr_session.lock_shared();
		float rate = 0;
		if (xr_session.get() && XR_SUCCEEDED(xr_ext->xrGetDisplayRefreshRateFB(xr_session.get(), &rate)) && rate > 0)
			return rate;
	}
	BackendManager* manager = BackendManager::InstancePtr();
	auto* backend = manager ? (XrBackend*)manager->GetBackendInstance() : nullptr;
	float periodMs = backend ? backend->DisplayPeriodMs() : 0.0f;
	if (periodMs > 0.0f)
		return 1000.0f / periodMs;
	return 90.0f;
}

float XrHMD::GetFloatTrackedDeviceProperty(vr::ETrackedDeviceProperty prop, vr::ETrackedPropertyError* pErrorL)
{
	if (pErrorL)
		*pErrorL = vr::TrackedProp_Success;

	TRY_PROFILE_PROP(float);

	switch (prop) {
	case vr::Prop_DeviceBatteryPercentage_Float: {
		std::optional<LibMonado::Battery> battery = LibMonado::Get().BatteryForRole("head");
		if (battery && battery->present)
			return battery->charge;
		break;
	}
	case vr::Prop_DisplayFrequency_Float:
		return DisplayFrequency();
	case vr::Prop_LensCenterLeftU_Float:
	case vr::Prop_LensCenterLeftV_Float:
	case vr::Prop_LensCenterRightU_Float:
	case vr::Prop_LensCenterRightV_Float:
		// SteamVR reports it as unknown
		if (pErrorL)
			*pErrorL = vr::TrackedProp_UnknownProperty;
		return 0;
	case vr::Prop_UserIpdMeters_Float:
		return BaseSystem::SGetIpd();
	case vr::Prop_SecondsFromVsyncToPhotons_Float:
		// Seems to be used by croteam games, IDK what the real value is, 100us should do
		return 0.0001f;
	case vr::Prop_UserHeadToEyeDepthMeters_Float:
		// The eye relief adjustment the user made in SteamVR's settings: none here
		return 0;
	default:
		break;
	}

	return XrTrackedDevice::GetFloatTrackedDeviceProperty(prop, pErrorL);
}

uint32_t XrHMD::GetStringTrackedDeviceProperty(vr::ETrackedDeviceProperty prop,
    char* value, uint32_t bufferSize, vr::ETrackedPropertyError* pErrorL)
{
	if (pErrorL)
		*pErrorL = vr::TrackedProp_Success;

	// From the HMD's row (Identity): the runtime's system, not the controllers' interaction profile
	return XrTrackedDevice::GetStringTrackedDeviceProperty(prop, value, bufferSize, pErrorL);
}

DeviceIdentity XrHMD::Identity()
{
	// The OpenXR system's name picks the row; read once per instance (it doesn't change while the instance lives)
	static std::mutex nameMutex;
	static XrInstance nameInstance = XR_NULL_HANDLE;
	static std::string systemName;
	{
		std::lock_guard<std::mutex> guard(nameMutex);
		if (xr_instance == XR_NULL_HANDLE) {
			systemName.clear();
			nameInstance = XR_NULL_HANDLE;
		} else if (xr_instance != nameInstance || systemName.empty()) {
			XrSystemProperties properties = { XR_TYPE_SYSTEM_PROPERTIES };
			systemName = XR_SUCCEEDED(xrGetSystemProperties(xr_instance, xr_system, &properties)) ? properties.systemName : "";
			nameInstance = xr_instance;
		}
	}
	RuntimeDevice device;
	if (std::optional<LibMonado::DeviceInfo> head = LibMonado::Get().DeviceInfoForRole("head")) {
		device.name = head->name;
		device.serial = head->serial;
	}
	std::string name;
	{
		std::lock_guard<std::mutex> guard(nameMutex);
		name = systemName;
	}
	return HmdIdentity(name, device);
}

int32_t XrHMD::GetInt32TrackedDeviceProperty(vr::ETrackedDeviceProperty prop, vr::ETrackedPropertyError* pErrorL)
{
	if (pErrorL)
		*pErrorL = vr::TrackedProp_Success;

	TRY_PROFILE_PROP(int32_t);

	switch (prop) {
	case vr::Prop_DeviceClass_Int32:
		return vr::TrackedDeviceClass_HMD;
	case vr::Prop_ExpectedControllerCount_Int32:
		return 2;
	case vr::Prop_ExpectedTrackingReferenceCount_Int32:
	case vr::Prop_NumCameras_Int32:
		return 0;
	default:
		break;
	}

	return ITrackedDevice::GetInt32TrackedDeviceProperty(prop, pErrorL);
}

uint64_t XrHMD::GetUint64TrackedDeviceProperty(vr::ETrackedDeviceProperty prop, vr::ETrackedPropertyError* pErrorL)
{
	uint64_t luid = 0;
	if (prop == vr::Prop_GraphicsAdapterLuid_Uint64 && BaseSystem::_RuntimeAdapterLuid(&luid)) {
		if (pErrorL)
			*pErrorL = vr::TrackedProp_Success;
		return luid;
	}
	return XrTrackedDevice::GetUint64TrackedDeviceProperty(prop, pErrorL);
}

void XrHMD::SetInteractionProfile(const InteractionProfile* profile)
{
	std::unique_lock lock(profile_mutex);
	this->profile = profile;
}
