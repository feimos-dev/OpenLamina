#include "stdafx.h"
#include <cmath>
#include <filesystem>
#include <openxr/openxr.h>
#define BASE_IMPL
#include "../../DrvOpenXR/CpuSwapchain.h"
#include "../../DrvOpenXR/XrBackend.h"
#include "BaseCompositor.h"
#include "Misc/FrameStats.h"
#include "BaseOverlay.h"

#include "Misc/EnumNames.h"
#include "BaseSystem.h"
#include "Compositor/compositor.h"
#include "Drivers/Backend.h"
#include "Misc/AppManifest.h"
#include "Misc/Config.h"
#include "Misc/Image.h"
#include "Misc/Input/InteractionProfile.h"
#include "Misc/OverlayMaths.h"
#include "Misc/ScopeGuard.h"
#include "convert.h"
#include "generated/static_bases.gen.h"
#include <optional>
#include <string>

using glm::mat4;
using glm::vec3;

using namespace vr;

// Class to represent an overlay
class BaseOverlay::OverlayData {
public:
	const string key;
	string name;
	HmdColor_t colour;

	float widthMeters = 1; // default 1 meter
	float curvature = 0;

	float autoCurveDistanceRangeMin, autoCurveDistanceRangeMax; // WTF does this do?
	EColorSpace colourSpace = ColorSpace_Auto;
	bool visible = false; // TODO check against SteamVR
	VRTextureBounds_t textureBounds = { 0, 0, 1, 1 };
	VROverlayInputMethod inputMethod = VROverlayInputMethod_None; // TODO fire events
	HmdVector2_t mouseScale = { 1.0f, 1.0f };
	bool highQuality = false;
	uint64_t flags = 0; // as masks: flag n is 1 << n
	float texelAspect = 1;
	float gamma = 1.0f; // IVROverlay_001-006
	int visibility = 0; // IVROverlay_001's VROverlayVisibility
	uint32_t systemOverlaySceneProcess = 0; // IVROverlay_001
	std::queue<VREvent_t> eventQueue;

	// Kept and returned, not shown yet (drawing or using them is not implemented yet)
	uint32_t renderingPid = 0;
	float preCurvePitch = 0;
	std::string renderModel;
	HmdColor_t renderModelColor = { 1, 1, 1, 1 };
	TrackedDeviceIndex_t componentDevice = k_unTrackedDeviceIndexInvalid;
	std::string componentName;
	VROverlayHandle_t relativeParent = k_ulOverlayHandleInvalid;
	HmdMatrix34_t relativeTransform = { { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } } };
	HmdVector2_t cursorHotspot = { 0, 0 };
	VROverlayHandle_t cursor = k_ulOverlayHandleInvalid;
	bool cursorOverridden = false;
	HmdVector2_t cursorOverride = { 0, 0 };
	std::map<int, VROverlayHandle_t> neighbours; // EOverlayDirection → overlay
	std::map<int, std::pair<HmdVector2_t, float>> dualAnalog; // EDualAnalogWhich → (centre, radius)

	// Rendering
	Texture_t texture = {};

	union {
		XrCompositionLayerBaseHeader base;
		XrCompositionLayerQuad quad;
		XrCompositionLayerCylinderKHR cylinder;
	} compositionLayer;

	std::weak_ptr<Compositor> compositor;
	// SetOverlayRaw's image (empty for a texture overlay), shown through a swapchain filled from memory
	Image rawImage;
	std::unique_ptr<CpuSwapchain> raw;
	float distance;
	uint32_t sortOrder = 0;

	// Transform
	VROverlayTransformType transformType = VROverlayTransform_Absolute;
	union {
		struct {
			HmdMatrix34_t offset;
			TrackedDeviceIndex_t device;
		} deviceRelative;
	} transformData;

	glm::mat4 overlayTransform{
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, -1.01f,
		0.0f, 0.0f, 0.0f, 1.0f
	};

	OverlayData(string key, string name)
	    : key(key), name(name)
	{
		memset(&compositionLayer, 0, sizeof(compositionLayer));
	}

	// The origin SetOverlayTransformAbsolute (or SetOverlayTransformProjection) gave; -1 before one was given, when the
	// transform is in whatever tracking space the frame uses
	int absoluteOrigin = -1;
	// Placed relative to relativeParent (by relativeTransform); the type it reports stays the one it had
	bool overlayRelative = false;
	// SetOverlayTransformProjection: the eye's projection, and which eye sees the overlay
	OOVR_VROverlayProjection_t projection = { -1, 1, -1, 1 };
	vr::EVREye projectionEye = vr::Eye_Left;
	// SetOverlayIntersectionMask: empty for the whole overlay
	std::vector<OOVR_VROverlayIntersectionMaskPrimitive_t> intersectionMask;

	// The overlay's own transform in the tracking space, as an ordinary glm matrix, without any parent overlay's
	bool OwnTransform(vr::ETrackingUniverseOrigin trackingSpace, glm::mat4& out) const;

	// The overlay's transform in the tracking space, stored like overlayTransform (rows first: translation in [i][3]),
	// through its parent overlays (valid: the overlays that exist)
	bool ComputeOverlayTransform(vr::ETrackingUniverseOrigin trackingSpace, const std::set<OverlayData*>& valid, glm::mat4& out) const;

	// The texture's width over its height (a flipped texture is drawn with a negative height), 1 without one
	float AspectRatio() const
	{
		const XrExtent2Di& extent = compositionLayer.quad.subImage.imageRect.extent;
		const int32_t height = std::abs(extent.height);
		return height > 0 && extent.width > 0 ? (float)extent.width / (float)height : 1.0f;
	}

	// Where the overlay is drawn in the tracking space; false if it can't be drawn
	bool ComputePlacement(vr::ETrackingUniverseOrigin trackingSpace, const std::set<OverlayData*>& valid, overlay_maths::Placement& out) const;
};

// TODO don't pass around handles, as it will cause
// crashes when we should merely return VROverlayError_InvalidHandle
#define OVL (*((OverlayData**)pOverlayHandle))
#define USEH()                                                                        \
	OverlayData* overlay = (OverlayData*)ulOverlayHandle;                             \
	if (!overlay || !validOverlays.count(overlay) || !overlays.count(overlay->key)) { \
		return VROverlayError_InvalidHandle;                                          \
	}

// USEH() for a setter of what _BuildLayers reads (on the own frames' thread too): under the layers' lock
#define USEH_LOCKED()                                                    \
	std::lock_guard<std::recursive_mutex> layersGuard(layersLock); \
	USEH()

#define USEHB()                                                                       \
	OverlayData* overlay = (OverlayData*)ulOverlayHandle;                             \
	if (!overlay || !validOverlays.count(overlay) || !overlays.count(overlay->key)) { \
		return false;                                                                 \
	}

// k_unVROverlayMaxKeyLength and k_unVROverlayMaxNameLength, the same in every SDK version: with the terminator
static constexpr uint32_t kOverlayMaxKeyLength = 128;
static constexpr uint32_t kOverlayMaxNameLength = 128;

// The overlay that has gamepad focus (SetGamepadFocusOverlay)
static VROverlayHandle_t gamepadFocusOverlay = k_ulOverlayHandleInvalid;

BaseOverlay::~BaseOverlay()
{
	// The session lock before the layers' lock, as everywhere: freeing a swapchain from memory takes it
	auto session = xr_session.lock_shared();
	std::lock_guard<std::recursive_mutex> lock(layersLock);
	for (const auto& kv : overlays) {
		if (kv.second) {
			delete kv.second;
		}
	}
}

// Overlays curved less than this are drawn flat: the bend is under a millimetre per metre of width, and the
// cylinder's radius grows so large that it loses precision
static constexpr float minOverlayCurvature = 1e-3f;

/**
 * Splits an overlay transform, stored like OverlayData::overlayTransform (rows first), into a quad's orientation and
 * the scale of its X and Y axes. A quad only uses its X and Y axes, so Z is made from them: that way every texel lands
 * where the transform puts it, mirrored or not (a mirrored overlay just faces the other way). Returns false if X or Y
 * is zero or they're parallel (a degenerate transform, such as one built from an untracked device's empty pose).
 */
static bool QuatAndScaleFromMatrix(glm::quat* orientation, const glm::mat4& mat, float* xscale, float* yscale)
{
	// The overlay's X and Y axes in the tracking space are the first two columns of the 3x3 part
	glm::vec3 x = glm::vec3(mat[0][0], mat[1][0], mat[2][0]);
	glm::vec3 y = glm::vec3(mat[0][1], mat[1][1], mat[2][1]);
	*xscale = glm::length(x);
	*yscale = glm::length(y);
	if (!(*xscale > 1e-6f) || !(*yscale > 1e-6f)) // also catches NaN
		return false;
	x /= *xscale;
	y /= *yscale;

	glm::vec3 z = glm::cross(x, y);
	const float zLength = glm::length(z);
	if (!(zLength > 1e-6f))
		return false;
	z /= zLength;
	y = glm::cross(z, x); // square off any shear

	// glm::mat3 takes columns
	*orientation = glm::normalize(glm::quat_cast(glm::mat3(x, y, z)));
	return true;
}

static bool IsFinite(const glm::vec3& v)
{
	return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool BaseOverlay::_AnyVisible()
{
	std::lock_guard<std::recursive_mutex> lock(layersLock);
	for (const auto& kv : overlays) {
		if (kv.second && kv.second->visible && (kv.second->texture.handle != nullptr || !kv.second->rawImage.Empty()))
			return true;
	}
	return false;
}

// The seated origin in the standing one (none if the runtime can't say)
static XrPosef SeatedInStanding()
{
	const XrPosef none{ { 0, 0, 0, 1 }, { 0, 0, 0 } };
	auto sessionLock = xr_session.lock_shared(); // held while reading: a session restart waits for it
	if (!xr_gbl)
		return none;
	XrSpaceLocation location{ XR_TYPE_SPACE_LOCATION };
	OC_XR_SOFT(xrLocateSpace(xr_gbl->seatedSpace, xr_gbl->floorSpace, xr_gbl->GetBestTime(), &location), none);
	const XrSpaceLocationFlags valid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	return (location.locationFlags & valid) == valid ? location.pose : none;
}

// A tracked device's component within the device: the interaction profile's transform, where it has one for the
// component (else the overlay is placed at the device itself)
static std::optional<glm::mat4> ComponentInDevice(ITrackedDevice& device, const std::string& component)
{
	const InteractionProfile* profile = device.GetInteractionProfile();
	if (!profile || component.empty())
		return {};
	return profile->GetComponentTransform(device.GetHand(), component);
}

// Where a cursor overlay's laser pointer meets the overlay it points at; nothing while nothing points at one
static std::optional<XrPosef> CursorPoint()
{
	return {};
}

bool BaseOverlay::OverlayData::OwnTransform(vr::ETrackingUniverseOrigin trackingSpace, glm::mat4& out) const
{
	if (transformType == VROverlayTransform_TrackedDeviceRelative || transformType == VROverlayTransform_TrackedComponent) {
		const bool component = transformType == VROverlayTransform_TrackedComponent;
		std::shared_ptr<ITrackedDevice> dev = BackendManager::Instance().GetDevice(component ? componentDevice : transformData.deviceRelative.device);
		if (!dev)
			return false;

		TrackedDevicePose_t pose = {};
		dev->GetPose(trackingSpace, &pose, ETrackingStateType::TrackingStateType_Rendering);
		if (!pose.bPoseIsValid)
			return false;
		const glm::mat4 device = S2G_m34(pose.mDeviceToAbsoluteTracking);
		if (component) {
			std::optional<glm::mat4> within = ComponentInDevice(*dev, componentName);
			out = within ? device * *within : device;
		} else {
			out = device * S2G_m34(transformData.deviceRelative.offset);
		}
		return true;
	}

	// Absolute, a projection's eye, or a cursor's own transform: overlayTransform is stored rows first
	out = glm::transpose(overlayTransform);
	if (transformType == VROverlayTransform_Cursor) {
		if (std::optional<XrPosef> point = CursorPoint()) {
			const XrPosef centre = overlay_maths::CursorPose(*point, widthMeters, widthMeters / AspectRatio(), cursorHotspot);
			out = X2G_om34_pose(centre);
			return true;
		}
	}
	// (raw is standing: only seated to anything else, or back, moves it)
	if (absoluteOrigin >= 0 && (absoluteOrigin == vr::TrackingUniverseSeated) != (trackingSpace == vr::TrackingUniverseSeated)) {
		const XrPosef none{ { 0, 0, 0, 1 }, { 0, 0, 0 } };
		const XrPosef shift = overlay_maths::ChangeOrigin(none, (vr::ETrackingUniverseOrigin)absoluteOrigin, trackingSpace, SeatedInStanding());
		out = X2G_om34_pose(shift) * out;
	}
	return true;
}

bool BaseOverlay::OverlayData::ComputeOverlayTransform(vr::ETrackingUniverseOrigin trackingSpace, const std::set<OverlayData*>& valid, glm::mat4& out) const
{
	using namespace overlay_maths;
	auto link = [&](const void* node) {
		ChainLink l;
		OverlayData* o = (OverlayData*)node;
		if (!valid.count(o))
			return l; // a parent that's gone
		if (o->overlayRelative) {
			l.relative = true;
			l.parent = (const void*)o->relativeParent;
			l.transform = S2G_m34(o->relativeTransform);
		} else {
			l.placed = o->OwnTransform(trackingSpace, l.transform);
		}
		return l;
	};
	glm::mat4 transform;
	switch (ResolveChain(this, link, transform)) {
	case Chain::Placed:
		out = glm::transpose(transform);
		return true;
	case Chain::Cycle:
		OC_UNSUPPORTED(false, "Overlay '%s' is relative to itself through its parents: hidden", key.c_str());
	case Chain::TooDeep:
		OC_UNSUPPORTED(false, "Overlay '%s' has more than %d parent overlays: hidden", key.c_str(), kMaxChainDepth);
	default:
		return false;
	}
}

bool BaseOverlay::OverlayData::ComputePlacement(vr::ETrackingUniverseOrigin trackingSpace, const std::set<OverlayData*>& valid, overlay_maths::Placement& out) const
{
	glm::mat4 transform = glm::mat4(0.0f);
	if (!ComputeOverlayTransform(trackingSpace, valid, transform))
		return false;
	float xscale, yscale;
	glm::quat orientation;
	const glm::vec3 position = { transform[0][3], transform[1][3], transform[2][3] };
	if (!QuatAndScaleFromMatrix(&orientation, transform, &xscale, &yscale) || !IsFinite(position))
		return false;
	const XrPosef pose{ G2X_quat(orientation), G2X_v3f(position) };

	if (transformType == VROverlayTransform_Projection && !overlayRelative) {
		out = overlay_maths::ProjectionQuad(pose, projection.fLeft, projection.fRight, projection.fTop, projection.fBottom);
		return true;
	}

	// As tall as the texture's aspect ratio makes it. Not worked out when the texture is set: the app may change the
	// width without changing the texture.
	const float width = widthMeters * xscale;
	const float height = widthMeters / AspectRatio() * yscale;
	const bool curved = curvature >= minOverlayCurvature && xr_ext->CylinderKHR_Available()
	    && std::isfinite(width) && std::isfinite(height) && width > 0 && height > 0;
	out = overlay_maths::Placement{ pose, width, height, curved ? curvature : 0.0f };
	return true;
}

int BaseOverlay::_BuildLayers(XrCompositionLayerBaseHeader* sceneLayer, XrCompositionLayerBaseHeader const* const*& layers, bool mayUpload)
{
	// Note that at least on MSVC, this shouldn't be doing any memory allocations
	//  unless the list is expanding from new layers.
	layerHeaders.clear();
	if (sceneLayer)
		layerHeaders.push_back(sceneLayer);

	// For performance reasons, this indicates that something is using input
	// Don't write it directly to usingInput, as that could cause race conditions between
	//  when it's initially set to false, and when it's reset to true.
	bool checkUsingInput = false;

	if (keyboard) {
#ifndef OC_XR_PORT
		layerHeaders.push_back(keyboard->Update());
#endif
		checkUsingInput = true;

		if (keyboard->IsClosed())
			HideKeyboard();
	}

	// sort overlays by distance starting with this index
	size_t firstOverlay = layerHeaders.size();
	// find HMD position to sort overlays by distance
	TrackedDevicePose_t hmdPose = {};
	vr::ETrackingUniverseOrigin trackingSpace = GetUnsafeBaseCompositor()->GetTrackingSpace();
	glm::vec3 hmdPosition = { 0.0f, 0.0f, 0.0f };
	std::shared_ptr<ITrackedDevice> hmd = BackendManager::Instance().GetDevice(vr::k_unTrackedDeviceIndex_Hmd);
	if (hmd) {
		hmd->GetPose(trackingSpace, &hmdPose, ETrackingStateType::TrackingStateType_Rendering);
		if (hmdPose.bPoseIsValid) {
			const auto& hmdM = hmdPose.mDeviceToAbsoluteTracking.m;
			hmdPosition = { hmdM[0][3], hmdM[1][3], hmdM[2][3] };
		}
	}
	layerOverlays.clear();

	for (const auto& kv : overlays) {
		if (kv.second) {
			OverlayData& overlay = *kv.second;

			// Skip hiddden overlays, and those without a valid texture or image (eg, after calling ClearOverlayTexture).
			if (!overlay.visible || (overlay.texture.handle == nullptr && overlay.rawImage.Empty()))
				continue;

			// An image from memory whose swapchain went with the last session is put into one on this session, by the
			// game's thread: the upload uses the device's immediate context or queue, which it uses too
			if (!overlay.rawImage.Empty()) {
				if (mayUpload && overlay.raw && !overlay.raw->Live())
					overlay.raw->Upload(overlay.rawImage);
				overlay.compositionLayer.quad.subImage = overlay.raw && overlay.raw->Live() ? overlay.raw->SubImage() : XrSwapchainSubImage{};
			}

			// Quick hack to get around Boneworks creating overlays and setting them to an opacity of
			// zero to hide them. Leave 1% of margin in case of weird float issues.
			// if (overlay.colour.a < 0.01)
			//	continue;

			if ((uint64_t)overlay.compositionLayer.quad.subImage.swapchain == 0) {
				continue;
			}

			const XrRect2Di& srcSize = overlay.compositionLayer.quad.subImage.imageRect;
			const int32_t srcHeight = std::abs(srcSize.extent.height);
			if (srcHeight <= 8 && srcSize.extent.width <= 8) {
				// Hack for F1 22 which creates a low res texture to fade between scenes
				// but ends up just leaving a black square that takes up half the screen.
				continue;
			}

			// Skip overlay if we can't compute where it goes
			overlay_maths::Placement placement;
			if (!overlay.ComputePlacement(trackingSpace, validOverlays, placement))
				continue;
			const glm::vec3 position = X2G_v3f(placement.pose.position);
			const glm::quat orientation = X2G_quat(placement.pose.orientation);

			// The transform is in the current tracking space, which the app may have changed since it set the texture
			overlay.compositionLayer.base.space = xr_space_from_ref_space_type(BaseSystem::currentSpace);
			// (eyeVisibility is the same for quad and cylinder layers)
			overlay.compositionLayer.quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
			if (overlay.transformType == VROverlayTransform_Projection && !overlay.overlayRelative)
				overlay.compositionLayer.quad.eyeVisibility = overlay.projectionEye == vr::Eye_Right ? XR_EYE_VISIBILITY_RIGHT : XR_EYE_VISIBILITY_LEFT;

			if (placement.curvature > 0) {
				// An OpenXR cylinder's visible arc is centred on its -Z, so its centre is a radius in front of the overlay
				const float radius = overlay_maths::CylinderRadius(placement.width, placement.curvature);
				const glm::vec3 centre = position + orientation * glm::vec3(0.0f, 0.0f, radius);
				overlay.compositionLayer.base.type = XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR;
				overlay.compositionLayer.cylinder.radius = radius;
				overlay.compositionLayer.cylinder.centralAngle = overlay_maths::CylinderAngle(placement.curvature);
				overlay.compositionLayer.cylinder.aspectRatio = placement.width / placement.height;
				overlay.compositionLayer.cylinder.pose = { placement.pose.orientation, G2X_v3f(centre) };
			} else {
				overlay.compositionLayer.base.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
				overlay.compositionLayer.quad.size.width = placement.width;
				overlay.compositionLayer.quad.size.height = placement.height;
				overlay.compositionLayer.quad.pose = placement.pose;
			}

			overlay.distance = glm::distance(position, hmdPosition);
			size_t i;

			// Sort the overlays: later layers are drawn on top, so by sort order, then the farthest first
			for (i = firstOverlay; i < layerHeaders.size(); i++) {
				const OverlayData* d = layerOverlays[i - firstOverlay];
				if (d->sortOrder > overlay.sortOrder)
					break;
				if (d->sortOrder == overlay.sortOrder && d->distance < overlay.distance)
					break;
			}
			// Finally, add it to the list of layers to be sent to LibOVR
			layerHeaders.insert(layerHeaders.begin() + i, &overlay.compositionLayer.base);
			layerOverlays.insert(layerOverlays.begin() + (i - firstOverlay), &overlay);
		}
	}

	usingInput = checkUsingInput;
	layers = layerHeaders.data();
	return static_cast<int>(layerHeaders.size());
}

bool BaseOverlay::_HandleOverlayInput(EVREye side, TrackedDeviceIndex_t index, VRControllerState_t state)
{
	if (!usingInput)
		return true;

	if (!keyboard)
		return true;

#ifndef OC_XR_PORT
	keyboard->HandleOverlayInput(side, state, (float)ovr_GetTimeInSeconds());
#endif
	return false;
}

EVROverlayError BaseOverlay::FindOverlay(const char* pchOverlayKey, VROverlayHandle_t* pOverlayHandle)
{
	if (!pchOverlayKey || !pOverlayHandle)
		return VROverlayError_InvalidParameter;
	*pOverlayHandle = k_ulOverlayHandleInvalid;
	if (overlays.count(pchOverlayKey)) {
		OVL = overlays[pchOverlayKey];
		return VROverlayError_None;
	}

	// TODO is this the correct return value
	return VROverlayError_InvalidParameter;
}
EVROverlayError BaseOverlay::CreateOverlay(const char* pchOverlayKey, const char* pchOverlayName, VROverlayHandle_t* pOverlayHandle)
{
	if (!pchOverlayKey || !pchOverlayName || !pOverlayHandle)
		return VROverlayError_InvalidParameter;
	*pOverlayHandle = k_ulOverlayHandleInvalid;
	// The SDK's limits, including the terminator
	if (strlen(pchOverlayKey) >= kOverlayMaxKeyLength)
		return VROverlayError_KeyTooLong;
	if (strlen(pchOverlayName) >= kOverlayMaxNameLength)
		return VROverlayError_NameTooLong;
	if (overlays.count(pchOverlayKey)) {
		return VROverlayError_KeyInUse;
	}

	std::lock_guard<std::recursive_mutex> lock(layersLock);
	OverlayData* data = new OverlayData(pchOverlayKey, pchOverlayName);
	OVL = data;

	overlays[pchOverlayKey] = data;
	validOverlays.insert(data);

	data->compositionLayer.base.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
	data->compositionLayer.base.next = NULL;
	data->compositionLayer.base.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
	data->compositionLayer.base.space = xr_space_from_ref_space_type(BaseSystem::currentSpace);

	// Still same for quad + cylinder
	data->compositionLayer.quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	data->compositionLayer.quad.pose = { { 0.f, 0.f, 0.f, 1.f },
		{ 0.0f, 0.0f, -0.65f } };

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::DestroyOverlay(VROverlayHandle_t ulOverlayHandle)
{
	USEH();

	if (highQualityOverlay == ulOverlayHandle)
		highQualityOverlay = vr::k_ulOverlayHandleInvalid;

	auto session = xr_session.lock_shared();
	std::lock_guard<std::recursive_mutex> lock(layersLock);
	if (std::shared_ptr<Compositor> comp = overlay->compositor.lock()) {
		auto* backend = (XrBackend*)BackendManager::Instance().GetBackendInstance();
		backend->UnregisterOverlayCompositor(comp);
	}
	overlays.erase(overlay->key);
	validOverlays.erase(overlay);
	delete overlay;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetHighQualityOverlay(VROverlayHandle_t ulOverlayHandle)
{
	USEH();

	highQualityOverlay = ulOverlayHandle;

	return VROverlayError_None;
}
VROverlayHandle_t BaseOverlay::GetHighQualityOverlay()
{
	if (!highQualityOverlay)
		return k_ulOverlayHandleInvalid;

	return highQualityOverlay;
}
// Copies value into an app's buffer of size bytes, cut short if it doesn't fit and always terminated, and returns
// the size needed including the terminator (as SteamVR does)
static uint32_t CopyOutString(const std::string& value, char* buffer, uint32_t size)
{
	if (buffer && size) {
		size_t n = std::min<size_t>(value.size(), size - 1);
		memcpy(buffer, value.data(), n);
		buffer[n] = 0;
	}
	return (uint32_t)value.size() + 1;
}

uint32_t BaseOverlay::GetOverlayKey(VROverlayHandle_t ulOverlayHandle, char* pchValue, uint32_t unBufferSize, EVROverlayError* pError)
{
	OverlayData* overlay = (OverlayData*)ulOverlayHandle;
	if (!overlay || !validOverlays.count(overlay) || !overlays.count(overlay->key)) {
		if (pError)
			*pError = VROverlayError_InvalidHandle;
		CopyOutString("", pchValue, unBufferSize);
		return 0;
	}
	if (pError)
		*pError = VROverlayError_None;
	return CopyOutString(overlay->key, pchValue, unBufferSize);
}
uint32_t BaseOverlay::GetOverlayName(VROverlayHandle_t ulOverlayHandle, VR_OUT_STRING() char* pchValue, uint32_t unBufferSize, EVROverlayError* pError)
{
	OverlayData* overlay = (OverlayData*)ulOverlayHandle;
	if (!overlay || !validOverlays.count(overlay) || !overlays.count(overlay->key)) {
		if (pError)
			*pError = VROverlayError_InvalidHandle;
		CopyOutString("", pchValue, unBufferSize);
		return 0;
	}
	if (pError)
		*pError = VROverlayError_None;
	return CopyOutString(overlay->name, pchValue, unBufferSize);
}
EVROverlayError BaseOverlay::SetOverlayName(VROverlayHandle_t ulOverlayHandle, const char* pchName)
{
	USEH();
	if (!pchName)
		return VROverlayError_InvalidParameter;
	if (strlen(pchName) >= kOverlayMaxNameLength)
		return VROverlayError_NameTooLong;

	overlay->name = pchName;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayImageData(VROverlayHandle_t ulOverlayHandle, void* pvBuffer, uint32_t unBufferSize, uint32_t* punWidth, uint32_t* punHeight)
{
	USEH();
	if (punWidth)
		*punWidth = 0;
	if (punHeight)
		*punHeight = 0;

	// The last image from memory, or the texture read back: what's needed is copied under the layers' lock, which
	// SetOverlayRaw and SetOverlayTexture write them under
	Image copied;
	Texture_t texture{};
	VRTextureBounds_t textureBounds{};
	std::shared_ptr<Compositor> shared;
	{
		std::lock_guard<std::recursive_mutex> lock(layersLock);
		copied = overlay->rawImage;
		texture = overlay->texture;
		textureBounds = overlay->textureBounds;
		shared = overlay->compositor.lock();
	}
	const Image* image = &copied;
	if (image->Empty()) {
		if (!texture.handle)
			return VROverlayError_InvalidTexture;
		std::unique_ptr<Compositor> own;
		Compositor* comp = shared.get();
		if (!comp) {
			own = BaseCompositor::CreateCompositorAPI(&texture);
			comp = own.get();
		}
		if (!comp || !comp->ReadbackToCpu(&texture, &textureBounds, copied))
			OC_UNSUPPORTED(VROverlayError_RequestFailed, "GetOverlayImageData: this overlay's texture (type %d) can't be read back", (int)texture.eType);
	}
	if (punWidth)
		*punWidth = image->width;
	if (punHeight)
		*punHeight = image->height;
	if (!pvBuffer || unBufferSize < image->rgba.size())
		return VROverlayError_ArrayTooSmall;
	memcpy(pvBuffer, image->rgba.data(), image->rgba.size());
	return VROverlayError_None;
}
const char* BaseOverlay::GetOverlayErrorNameFromEnum(EVROverlayError error)
{
	// As SteamVR answers
	return OC_EnumNameAsSteamVR("EVROverlayError", (int32_t)error, "Unknown EVROverlayError (%d)");
}
EVROverlayError BaseOverlay::SetOverlayRenderingPid(VROverlayHandle_t ulOverlayHandle, uint32_t unPID)
{
	USEH();
	overlay->renderingPid = unPID;
	return VROverlayError_None;
}
uint32_t BaseOverlay::GetOverlayRenderingPid(VROverlayHandle_t ulOverlayHandle)
{
	OverlayData* overlay = (OverlayData*)ulOverlayHandle;
	if (!overlay || !validOverlays.count(overlay) || !overlays.count(overlay->key))
		return 0;
	return overlay->renderingPid;
}
EVROverlayError BaseOverlay::SetOverlayFlag(VROverlayHandle_t ulOverlayHandle, VROverlayFlags eOverlayFlag, bool bEnabled)
{
	USEH_LOCKED();

	if (bEnabled) {
		overlay->flags |= 1uLL << eOverlayFlag;
	} else {
		overlay->flags &= ~(1uLL << eOverlayFlag);
	}

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayFlag(VROverlayHandle_t ulOverlayHandle, VROverlayFlags eOverlayFlag, bool* pbEnabled)
{
	USEH();

	*pbEnabled = (overlay->flags & (1uLL << eOverlayFlag)) != 0uLL;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayColor(VROverlayHandle_t ulOverlayHandle, float fRed, float fGreen, float fBlue)
{
	USEH_LOCKED();

	overlay->colour.r = fRed;
	overlay->colour.g = fGreen;
	overlay->colour.b = fBlue;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayColor(VROverlayHandle_t ulOverlayHandle, float* pfRed, float* pfGreen, float* pfBlue)
{
	USEH();

	*pfRed = overlay->colour.r;
	*pfGreen = overlay->colour.g;
	*pfBlue = overlay->colour.b;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayAlpha(VROverlayHandle_t ulOverlayHandle, float fAlpha)
{
	USEH_LOCKED();

	overlay->colour.a = fAlpha;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayAlpha(VROverlayHandle_t ulOverlayHandle, float* pfAlpha)
{
	USEH();

	*pfAlpha = overlay->colour.a;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayTexelAspect(VROverlayHandle_t ulOverlayHandle, float fTexelAspect)
{
	USEH();

	overlay->texelAspect = fTexelAspect;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayTexelAspect(VROverlayHandle_t ulOverlayHandle, float* pfTexelAspect)
{
	USEH();

	if (!pfTexelAspect)
		return VROverlayError_InvalidParameter;

	*pfTexelAspect = overlay->texelAspect;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlaySortOrder(VROverlayHandle_t ulOverlayHandle, uint32_t unSortOrder)
{
	USEH_LOCKED();
	overlay->sortOrder = unSortOrder;
	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlaySortOrder(VROverlayHandle_t ulOverlayHandle, uint32_t* punSortOrder)
{
	USEH();
	*punSortOrder = overlay->sortOrder;
	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayWidthInMeters(VROverlayHandle_t ulOverlayHandle, float fWidthInMeters)
{
	USEH_LOCKED();

	overlay->widthMeters = fWidthInMeters;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayWidthInMeters(VROverlayHandle_t ulOverlayHandle, float* pfWidthInMeters)
{
	USEH();

	*pfWidthInMeters = overlay->widthMeters;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayCurvature(VROverlayHandle_t ulOverlayHandle, float fCurvature)
{
	USEH_LOCKED();

	// _BuildLayers draws it as a cylinder where it can
	overlay->curvature = fCurvature;
	if (fCurvature >= minOverlayCurvature && !xr_ext->CylinderKHR_Available())
		OOVR_LOG_ONCE("WARNING: Tried to set overlay curvature, but XR_KHR_composition_layer_cylinder is not available");

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayCurvature(VROverlayHandle_t ulOverlayHandle, float* pfCurvature)
{
	USEH();

	*pfCurvature = overlay->curvature;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayAutoCurveDistanceRangeInMeters(VROverlayHandle_t ulOverlayHandle, float fMinDistanceInMeters, float fMaxDistanceInMeters)
{
	USEH();

	overlay->autoCurveDistanceRangeMin = fMinDistanceInMeters;
	overlay->autoCurveDistanceRangeMax = fMaxDistanceInMeters;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayAutoCurveDistanceRangeInMeters(VROverlayHandle_t ulOverlayHandle, float* pfMinDistanceInMeters, float* pfMaxDistanceInMeters)
{
	USEH();

	*pfMinDistanceInMeters = overlay->autoCurveDistanceRangeMin;
	*pfMaxDistanceInMeters = overlay->autoCurveDistanceRangeMax;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayTextureColorSpace(VROverlayHandle_t ulOverlayHandle, EColorSpace eTextureColorSpace)
{
	USEH_LOCKED();

	overlay->colourSpace = eTextureColorSpace;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayTextureColorSpace(VROverlayHandle_t ulOverlayHandle, EColorSpace* peTextureColorSpace)
{
	USEH();

	*peTextureColorSpace = overlay->colourSpace;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayTextureBounds(VROverlayHandle_t ulOverlayHandle, const VRTextureBounds_t* pOverlayTextureBounds)
{
	USEH_LOCKED();

	if (pOverlayTextureBounds)
		overlay->textureBounds = *pOverlayTextureBounds;
	else
		overlay->textureBounds = { 0, 0, 1, 1 };

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayTextureBounds(VROverlayHandle_t ulOverlayHandle, VRTextureBounds_t* pOverlayTextureBounds)
{
	USEH();

	*pOverlayTextureBounds = overlay->textureBounds;

	return VROverlayError_None;
}
uint32_t BaseOverlay::GetOverlayRenderModel(VROverlayHandle_t ulOverlayHandle, char* pchValue, uint32_t unBufferSize, HmdColor_t* pColor, EVROverlayError* pError)
{
	auto fail = [&](EVROverlayError e) -> uint32_t {
		if (pError)
			*pError = e;
		if (pchValue && unBufferSize)
			pchValue[0] = 0;
		return 0;
	};
	OverlayData* overlay = (OverlayData*)ulOverlayHandle;
	if (!overlay || !validOverlays.count(overlay) || !overlays.count(overlay->key))
		return fail(VROverlayError_InvalidHandle);
	if (pError)
		*pError = VROverlayError_None;
	if (pColor)
		*pColor = overlay->renderModelColor;
	uint32_t needed = (uint32_t)overlay->renderModel.size() + 1;
	if (pchValue && unBufferSize >= needed)
		memcpy(pchValue, overlay->renderModel.c_str(), needed);
	else if (pchValue && unBufferSize)
		pchValue[0] = 0;
	return needed;
}
EVROverlayError BaseOverlay::SetOverlayRenderModel(VROverlayHandle_t ulOverlayHandle, const char* pchRenderModel, const HmdColor_t* pColor)
{
	USEH();
	overlay->renderModel = pchRenderModel ? pchRenderModel : "";
	if (pColor)
		overlay->renderModelColor = *pColor;
	OOVR_LOG_ONCE("Overlay render models are kept but not drawn");
	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayTransformType(VROverlayHandle_t ulOverlayHandle, VROverlayTransformType* peTransformType)
{
	USEH();

	*peTransformType = overlay->transformType;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayTransformAbsolute(VROverlayHandle_t ulOverlayHandle, ETrackingUniverseOrigin eTrackingOrigin, const HmdMatrix34_t* pmatTrackingOriginToOverlayTransform)
{
	USEH_LOCKED();

	// Drawn in whatever origin the frame uses (ChangeOrigin)
	overlay->transformType = VROverlayTransform_Absolute;
	overlay->overlayRelative = false;
	overlay->absoluteOrigin = (int)eTrackingOrigin;
	S2O_om44(*pmatTrackingOriginToOverlayTransform, overlay->overlayTransform);

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayTransformAbsolute(VROverlayHandle_t ulOverlayHandle, ETrackingUniverseOrigin* peTrackingOrigin, HmdMatrix34_t* pmatTrackingOriginToOverlayTransform)
{
	USEH();

	if (overlay->transformType != VROverlayTransform_Absolute)
		return VROverlayError_WrongTransformType;

	if (peTrackingOrigin && overlay->absoluteOrigin >= 0)
		*peTrackingOrigin = (ETrackingUniverseOrigin)overlay->absoluteOrigin;
	O2S_om34(overlay->overlayTransform, *pmatTrackingOriginToOverlayTransform);

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayTransformTrackedDeviceRelative(VROverlayHandle_t ulOverlayHandle, TrackedDeviceIndex_t unTrackedDevice, const HmdMatrix34_t* pmatTrackedDeviceToOverlayTransform)
{
	USEH_LOCKED();

	overlay->transformType = VROverlayTransform_TrackedDeviceRelative;
	overlay->overlayRelative = false;
	overlay->transformData.deviceRelative.device = unTrackedDevice;
	overlay->transformData.deviceRelative.offset = *pmatTrackedDeviceToOverlayTransform;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayTransformTrackedDeviceRelative(VROverlayHandle_t ulOverlayHandle, TrackedDeviceIndex_t* punTrackedDevice, HmdMatrix34_t* pmatTrackedDeviceToOverlayTransform)
{
	USEH();
	if (!punTrackedDevice || !pmatTrackedDeviceToOverlayTransform)
		return VROverlayError_InvalidParameter;
	if (overlay->transformType != VROverlayTransform_TrackedDeviceRelative)
		return VROverlayError_WrongTransformType;
	*punTrackedDevice = overlay->transformData.deviceRelative.device;
	*pmatTrackedDeviceToOverlayTransform = overlay->transformData.deviceRelative.offset;
	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayTransformTrackedDeviceComponent(VROverlayHandle_t ulOverlayHandle, TrackedDeviceIndex_t unDeviceIndex, const char* pchComponentName)
{
	USEH_LOCKED();
	overlay->transformType = VROverlayTransform_TrackedComponent;
	overlay->overlayRelative = false;
	overlay->componentDevice = unDeviceIndex;
	overlay->componentName = pchComponentName ? pchComponentName : "";
	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayTransformTrackedDeviceComponent(VROverlayHandle_t ulOverlayHandle, TrackedDeviceIndex_t* punDeviceIndex, char* pchComponentName, uint32_t unComponentNameSize)
{
	USEH();
	if (overlay->transformType != VROverlayTransform_TrackedComponent)
		return VROverlayError_WrongTransformType;
	if (punDeviceIndex)
		*punDeviceIndex = overlay->componentDevice;
	if (pchComponentName && unComponentNameSize) {
		if (overlay->componentName.size() + 1 > unComponentNameSize) {
			pchComponentName[0] = 0;
			return VROverlayError_ArrayTooSmall;
		}
		memcpy(pchComponentName, overlay->componentName.c_str(), overlay->componentName.size() + 1);
	}
	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayTransformOverlayRelative(VROverlayHandle_t ulOverlayHandle, VROverlayHandle_t* ulOverlayHandleParent, HmdMatrix34_t* pmatParentOverlayToOverlayTransform)
{
	USEH();
	if (!ulOverlayHandleParent || !pmatParentOverlayToOverlayTransform)
		return VROverlayError_InvalidParameter;
	*ulOverlayHandleParent = overlay->relativeParent;
	*pmatParentOverlayToOverlayTransform = overlay->relativeTransform;
	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayTransformOverlayRelative(VROverlayHandle_t ulOverlayHandle, VROverlayHandle_t ulOverlayHandleParent, const HmdMatrix34_t* pmatParentOverlayToOverlayTransform)
{
	USEH();
	// Placed by its parent when drawn: the parent may be made, moved or destroyed afterwards
	overlay->overlayRelative = true;
	overlay->relativeParent = ulOverlayHandleParent;
	if (pmatParentOverlayToOverlayTransform)
		overlay->relativeTransform = *pmatParentOverlayToOverlayTransform;
	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayTransformCursor(VROverlayHandle_t ulCursorOverlayHandle, const HmdVector2_t* pvHotspot)
{
	OverlayData* overlay = (OverlayData*)ulCursorOverlayHandle;
	if (!overlay || !validOverlays.count(overlay) || !overlays.count(overlay->key))
		return VROverlayError_InvalidHandle;
	// Drawn where a laser pointer meets the overlay it's the cursor of; until one does, at its own transform
	overlay->transformType = VROverlayTransform_Cursor;
	overlay->overlayRelative = false;
	if (pvHotspot)
		overlay->cursorHotspot = *pvHotspot;
	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayTransformCursor(VROverlayHandle_t ulOverlayHandle, HmdVector2_t* pvHotspot)
{
	USEH();
	if (!pvHotspot)
		return VROverlayError_InvalidParameter;
	*pvHotspot = overlay->cursorHotspot;
	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayTransformProjection(VROverlayHandle_t ulOverlayHandle,
    ETrackingUniverseOrigin eTrackingOrigin, const HmdMatrix34_t* pmatTrackingOriginToOverlayTransform,
    const OOVR_VROverlayProjection_t* pProjection, EVREye eEye)
{
	USEH_LOCKED();
	if (!pmatTrackingOriginToOverlayTransform || !pProjection)
		return VROverlayError_InvalidParameter;
	// The transform is the eye's: the overlay covers its projection, 1 m ahead, for that eye alone
	overlay->transformType = VROverlayTransform_Projection;
	overlay->overlayRelative = false;
	overlay->absoluteOrigin = (int)eTrackingOrigin;
	S2O_om44(*pmatTrackingOriginToOverlayTransform, overlay->overlayTransform);
	overlay->projection = *pProjection;
	overlay->projectionEye = eEye == vr::Eye_Right ? vr::Eye_Right : vr::Eye_Left;
	return VROverlayError_None;
}
EVROverlayError BaseOverlay::ShowOverlay(VROverlayHandle_t ulOverlayHandle)
{
	USEH_LOCKED();
	overlay->visible = true;
	return VROverlayError_None;
}
EVROverlayError BaseOverlay::HideOverlay(VROverlayHandle_t ulOverlayHandle)
{
	USEH_LOCKED();
	overlay->visible = false;
	return VROverlayError_None;
}
bool BaseOverlay::IsOverlayVisible(VROverlayHandle_t ulOverlayHandle)
{
	USEHB();
	return overlay->visible;
}
EVROverlayError BaseOverlay::GetTransformForOverlayCoordinates(VROverlayHandle_t ulOverlayHandle, ETrackingUniverseOrigin eTrackingOrigin, HmdVector2_t coordinatesInOverlay, HmdMatrix34_t* pmatTransform)
{
	USEH();
	if (!pmatTransform)
		return VROverlayError_InvalidParameter;
	overlay_maths::Placement placement;
	if (!overlay->ComputePlacement(eTrackingOrigin, validOverlays, placement)) {
		O2S_om34(overlay->overlayTransform, *pmatTransform);
		OC_UNSUPPORTED(VROverlayError_None, "GetTransformForOverlayCoordinates: overlay '%s' can't be placed, answered with its own transform", overlay->key.c_str());
	}
	const XrPosef point = overlay_maths::PointOn(placement, coordinatesInOverlay.v[0], coordinatesInOverlay.v[1]);
	*pmatTransform = G2S_m34(X2G_om34_pose(point));
	return VROverlayError_None;
}
bool BaseOverlay::PollNextOverlayEvent(VROverlayHandle_t ulOverlayHandle, VREvent_t* pEvent, uint32_t eventSize)
{
	USEHB();

	memset(pEvent, 0, eventSize);

	if (overlay->eventQueue.empty())
		return false;

	VREvent_t e = overlay->eventQueue.front();
	overlay->eventQueue.pop();

	memcpy(pEvent, &e, std::min((uint32_t)sizeof(e), eventSize));

	return true;
}
EVROverlayError BaseOverlay::GetOverlayInputMethod(VROverlayHandle_t ulOverlayHandle, VROverlayInputMethod* peInputMethod)
{
	USEH();

	if (peInputMethod)
		*peInputMethod = overlay->inputMethod;

	return VROverlayError_None;
}

EVROverlayError BaseOverlay::SetOverlayInputMethod(VROverlayHandle_t ulOverlayHandle, VROverlayInputMethod eInputMethod)
{
	USEH();

	overlay->inputMethod = eInputMethod;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayMouseScale(VROverlayHandle_t ulOverlayHandle, HmdVector2_t* pvecMouseScale)
{
	USEH();

	*pvecMouseScale = overlay->mouseScale;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayMouseScale(VROverlayHandle_t ulOverlayHandle, const HmdVector2_t* pvecMouseScale)
{
	USEH();

	if (pvecMouseScale)
		overlay->mouseScale = *pvecMouseScale;
	else
		overlay->mouseScale = HmdVector2_t{ 1.0f, 1.0f };

	return VROverlayError_None;
}
bool BaseOverlay::ComputeOverlayIntersection(VROverlayHandle_t ulOverlayHandle, const OOVR_VROverlayIntersectionParams_t* pParams, OOVR_VROverlayIntersectionResults_t* pResults)
{
	USEHB();

	if (!pParams || !pResults)
		return false;
	overlay_maths::Placement placement;
	if (!overlay->ComputePlacement(pParams->eOrigin, validOverlays, placement))
		return false; // not drawn either

	const auto& s = pParams->vSource.v;
	const auto& d = pParams->vDirection.v;
	overlay_maths::Hit hit{};
	hit.distance = -1;
	const bool on = overlay_maths::Intersect(placement, { s[0], s[1], s[2] }, { d[0], d[1], d[2] }, hit);
	if (hit.distance < 0)
		return false; // not even the overlay's plane or cylinder
	pResults->vPoint = { hit.point.x, hit.point.y, hit.point.z };
	pResults->vNormal = { hit.normal.x, hit.normal.y, hit.normal.z };
	pResults->vUVs = { hit.u, hit.v };
	pResults->fDistance = hit.distance;
	// The mask is in the overlay's mouse coordinates, from its upper left
	const float x = hit.u * overlay->mouseScale.v[0];
	const float y = (1 - hit.v) * overlay->mouseScale.v[1];
	return on && overlay_maths::InMask(overlay->intersectionMask, x, y);
}
bool BaseOverlay::HandleControllerOverlayInteractionAsMouse(VROverlayHandle_t ulOverlayHandle, TrackedDeviceIndex_t unControllerDeviceIndex)
{
	OC_UNSUPPORTED(false, "HandleControllerOverlayInteractionAsMouse: no laser mouse yet");
}
EVROverlayError BaseOverlay::SetOverlayFlagMask(VROverlayHandle_t ulOverlayHandle, uint32_t flagMask, bool bEnabled)
{
	USEH();
	if (bEnabled)
		overlay->flags |= flagMask;
	else
		overlay->flags &= ~(uint64_t)flagMask;
	return VROverlayError_None;
}

EVROverlayError BaseOverlay::GetOverlayFlagMask(VROverlayHandle_t ulOverlayHandle, uint32_t flagMask, bool* pbEnabled)
{
	USEH();
	if (!pbEnabled)
		return VROverlayError_InvalidParameter;
	*pbEnabled = flagMask != 0 && (overlay->flags & flagMask) == flagMask;
	return VROverlayError_None;
}

EVROverlayError BaseOverlay::SetOverlayGamma(VROverlayHandle_t ulOverlayHandle, float fGamma)
{
	USEH();
	overlay->gamma = fGamma;
	return VROverlayError_None;
}

EVROverlayError BaseOverlay::GetOverlayGamma(VROverlayHandle_t ulOverlayHandle, float* pfGamma)
{
	USEH();
	if (!pfGamma)
		return VROverlayError_InvalidParameter;
	*pfGamma = overlay->gamma;
	return VROverlayError_None;
}

EVROverlayError BaseOverlay::SetOverlayVisibility(VROverlayHandle_t ulOverlayHandle, OOVR_VROverlayVisibility eOverlayVisibility)
{
	USEH();
	overlay->visibility = (int)eOverlayVisibility;
	return VROverlayError_None;
}

EVROverlayError BaseOverlay::GetOverlayVisibility(VROverlayHandle_t ulOverlayHandle, OOVR_VROverlayVisibility* peOverlayVisibility)
{
	USEH();
	if (!peOverlayVisibility)
		return VROverlayError_InvalidParameter;
	*peOverlayVisibility = (OOVR_VROverlayVisibility)overlay->visibility;
	return VROverlayError_None;
}

bool BaseOverlay::IsSystemOverlayVisible()
{
	// The system button opens the OpenXR runtime's own menu, not one of these
	return false;
}

bool BaseOverlay::IsActiveSystemOverlay(VROverlayHandle_t ulOverlayHandle)
{
	return false;
}

EVROverlayError BaseOverlay::SetSystemOverlaySceneProcess(VROverlayHandle_t ulOverlayHandle, uint32_t unProcessId)
{
	USEH();
	overlay->systemOverlaySceneProcess = unProcessId;
	return VROverlayError_None;
}

EVROverlayError BaseOverlay::GetSystemOverlaySceneProcess(VROverlayHandle_t ulOverlayHandle, uint32_t* punProcessId)
{
	USEH();
	if (!punProcessId)
		return VROverlayError_InvalidParameter;
	*punProcessId = overlay->systemOverlaySceneProcess;
	return VROverlayError_None;
}

bool BaseOverlay::PollNextOverlayEventOld(VROverlayHandle_t ulOverlayHandle, VREvent_t* pEvent)
{
	if (!pEvent)
		return false;
	memset(pEvent, 0, BaseSystem::kOldEventSize);
	VREvent_t event = {};
	if (!PollNextOverlayEvent(ulOverlayHandle, &event, sizeof(event)))
		return false;
	BaseSystem::WriteOldEvent(event, pEvent);
	return true;
}

EVROverlayError BaseOverlay::SetOverlayTextureForDevice(VROverlayHandle_t ulOverlayHandle, void* pTexture)
{
	Texture_t texture = { pTexture, TextureType_DirectX, ColorSpace_Auto };
	BaseCompositor* compositor = GetUnsafeBaseCompositor();
	if (!compositor || !compositor->OldDeviceTextureType(&texture.eType))
		OC_UNSUPPORTED(VROverlayError_InvalidTexture, "SetOverlayTexture (IVROverlay_001): IVRCompositor::SetGraphicsDevice "
		                                              "named no D3D11 or OpenGL device");
	return SetOverlayTexture(ulOverlayHandle, &texture);
}

EVROverlayError BaseOverlay::SetOverlayTextureWithConvention(VROverlayHandle_t ulOverlayHandle, EGraphicsAPIConvention eTextureType, void* pTexture)
{
	// API_DirectX and API_OpenGL are TextureType_DirectX and TextureType_OpenGL
	Texture_t texture = { pTexture, (ETextureType)eTextureType, ColorSpace_Auto };
	return SetOverlayTexture(ulOverlayHandle, &texture);
}

bool BaseOverlay::IsFocusOverlay(VROverlayHandle_t ulOverlayHandle)
{
	return IsHoverTargetOverlay(ulOverlayHandle);
}

bool BaseOverlay::IsHoverTargetOverlay(VROverlayHandle_t ulOverlayHandle)
{
	OC_UNSUPPORTED(false, "IsHoverTargetOverlay: no laser mouse yet");
}
VROverlayHandle_t BaseOverlay::GetGamepadFocusOverlay()
{
	return gamepadFocusOverlay;
}
EVROverlayError BaseOverlay::SetGamepadFocusOverlay(VROverlayHandle_t ulNewFocusOverlay)
{
	if (ulNewFocusOverlay != k_ulOverlayHandleInvalid) {
		OverlayData* overlay = (OverlayData*)ulNewFocusOverlay;
		if (!validOverlays.count(overlay))
			return VROverlayError_InvalidHandle;
	}
	gamepadFocusOverlay = ulNewFocusOverlay;
	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayNeighbor(EOverlayDirection eDirection, VROverlayHandle_t ulFrom, VROverlayHandle_t ulTo)
{
	OverlayData* overlay = (OverlayData*)ulFrom;
	if (!overlay || !validOverlays.count(overlay))
		return VROverlayError_InvalidHandle;
	if (ulTo == k_ulOverlayHandleInvalid)
		overlay->neighbours.erase((int)eDirection);
	else
		overlay->neighbours[(int)eDirection] = ulTo;
	return VROverlayError_None;
}
EVROverlayError BaseOverlay::MoveGamepadFocusToNeighbor(EOverlayDirection eDirection, VROverlayHandle_t ulFrom)
{
	OverlayData* overlay = (OverlayData*)ulFrom;
	if (!overlay || !validOverlays.count(overlay))
		return VROverlayError_InvalidHandle;
	auto it = overlay->neighbours.find((int)eDirection);
	if (it == overlay->neighbours.end())
		return VROverlayError_NoNeighbor;
	gamepadFocusOverlay = it->second;
	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayDualAnalogTransform(VROverlayHandle_t ulOverlay, EDualAnalogWhich eWhich, const HmdVector2_t& vCenter, float fRadius)
{
	OverlayData* overlay = (OverlayData*)ulOverlay;
	if (!overlay || !validOverlays.count(overlay))
		return VROverlayError_InvalidHandle;
	overlay->dualAnalog[(int)eWhich] = { vCenter, fRadius };
	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayDualAnalogTransform(VROverlayHandle_t ulOverlay, EDualAnalogWhich eWhich, HmdVector2_t* pvCenter, float* pfRadius)
{
	OverlayData* overlay = (OverlayData*)ulOverlay;
	if (!overlay || !validOverlays.count(overlay))
		return VROverlayError_InvalidHandle;
	auto it = overlay->dualAnalog.find((int)eWhich);
	if (pvCenter)
		*pvCenter = it == overlay->dualAnalog.end() ? HmdVector2_t{ 0, 0 } : it->second.first;
	if (pfRadius)
		*pfRadius = it == overlay->dualAnalog.end() ? 0.0f : it->second.second;
	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayDualAnalogTransform(VROverlayHandle_t ulOverlay, EDualAnalogWhich eWhich, const HmdVector2_t* pvCenter, float fRadius)
{
	if (!pvCenter)
		return VROverlayError_InvalidParameter;
	return SetOverlayDualAnalogTransform(ulOverlay, eWhich, *pvCenter, fRadius);
}
EVROverlayError BaseOverlay::TriggerLaserMouseHapticVibration(VROverlayHandle_t ulOverlayHandle, float fDurationSeconds, float fFrequency, float fAmplitude)
{
	USEH();
	OC_UNSUPPORTED(VROverlayError_None, "TriggerLaserMouseHapticVibration: no laser mouse yet");
}
EVROverlayError BaseOverlay::SetOverlayCursor(VROverlayHandle_t ulOverlayHandle, VROverlayHandle_t ulCursorHandle)
{
	USEH();
	overlay->cursor = ulCursorHandle;
	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayCursorPositionOverride(VROverlayHandle_t ulOverlayHandle, const HmdVector2_t* pvCursor)
{
	USEH();
	if (!pvCursor)
		return VROverlayError_InvalidParameter;
	overlay->cursorOverridden = true;
	overlay->cursorOverride = *pvCursor;
	return VROverlayError_None;
}
EVROverlayError BaseOverlay::ClearOverlayCursorPositionOverride(VROverlayHandle_t ulOverlayHandle)
{
	USEH();
	overlay->cursorOverridden = false;
	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayTexture(VROverlayHandle_t ulOverlayHandle, const Texture_t* pTexture)
{
	USEH();
	if (!pTexture)
		return VROverlayError_InvalidTexture;
	Texture_t texture = *pTexture;
	BackendManager::Instance().OnOverlayTexture(&texture);

	// After OnOverlayTexture, which may restart the session; the session lock before the layers' lock, as everywhere
	auto session = xr_session.lock_shared();
	std::lock_guard<std::recursive_mutex> lock(layersLock);
	overlay->texture = texture;
	// A texture replaces an image from memory
	overlay->raw.reset();
	overlay->rawImage = {};

	if (!BackendManager::Instance().IsGraphicsConfigured())
		return VROverlayError_None;

	std::shared_ptr<Compositor> compositor = overlay->compositor.lock();
	if (!compositor) {
		// Overlay apps needn't have asked for IVRCompositor
		compositor = BaseCompositor::CreateCompositorAPI(&overlay->texture);
		// No compositor for this texture type: nothing to draw
		if (!compositor)
			OC_UNSUPPORTED(VROverlayError_InvalidTexture, "SetOverlayTexture: no compositor for texture type %d", (int)overlay->texture.eType);
		overlay->compositor = std::weak_ptr(compositor);
		auto* backend = (XrBackend*)BackendManager::Instance().GetBackendInstance();
		backend->RegisterOverlayCompositor(compositor);
	}

	compositor->LoadSubmitContext();
	// (subImage is the same for quad and cylinder layers)
	compositor->Invoke(&overlay->texture, &overlay->textureBounds, overlay->compositionLayer.quad.subImage);
	compositor->ResetSubmitContext();

	overlay->compositionLayer.base.space = xr_space_from_ref_space_type(BaseSystem::currentSpace);

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::ClearOverlayTexture(VROverlayHandle_t ulOverlayHandle)
{
	USEH();
	auto session = xr_session.lock_shared();
	std::lock_guard<std::recursive_mutex> lock(layersLock);
	overlay->texture = {};
	overlay->raw.reset();
	overlay->rawImage = {};

	if (std::shared_ptr<Compositor> comp = overlay->compositor.lock()) {
		auto* backend = (XrBackend*)BackendManager::Instance().GetBackendInstance();
		backend->UnregisterOverlayCompositor(comp);
	}
	overlay->compositor.reset();
	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayRaw(VROverlayHandle_t ulOverlayHandle, void* pvBuffer, uint32_t unWidth, uint32_t unHeight, uint32_t unDepth)
{
	USEH();
	// One byte (grey), three (RGB) or four (RGBA) per pixel, rows top to bottom
	if (!pvBuffer || !unWidth || !unHeight || (unDepth != 1 && unDepth != 3 && unDepth != 4))
		return VROverlayError_InvalidParameter;
	Image image = ConvertToRgba8(pvBuffer, unWidth, unHeight, unWidth * unDepth, unDepth);
	if (image.Empty())
		return VROverlayError_InvalidParameter;

	// The overlay's image and swapchain are what OpenLamina's own frames draw from
	auto session = xr_session.lock_shared();
	std::lock_guard<std::recursive_mutex> lock(layersLock);

	// An image replaces a texture: shown as a texture overlay is, through its own swapchain
	if (std::shared_ptr<Compositor> comp = overlay->compositor.lock()) {
		auto* backend = (XrBackend*)BackendManager::Instance().GetBackendInstance();
		backend->UnregisterOverlayCompositor(comp);
	}
	overlay->compositor.reset();
	overlay->texture = {};
	overlay->rawImage = std::move(image);
	if (!overlay->raw)
		overlay->raw = CpuSwapchain::Create(unWidth, unHeight);
	if (!overlay->raw) {
		// Logged (a soft failure) by Create: no session with a graphics API
		overlay->rawImage = {};
		return VROverlayError_RequestFailed;
	}
	// A failed upload (logged) is tried again when the frame's layers are built
	if (overlay->raw->Upload(overlay->rawImage))
		overlay->compositionLayer.quad.subImage = overlay->raw->SubImage();
	overlay->compositionLayer.base.space = xr_space_from_ref_space_type(BaseSystem::currentSpace);
	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayFromFile(VROverlayHandle_t ulOverlayHandle, const char* pchFilePath)
{
	USEH();
	if (!pchFilePath || !*pchFilePath)
		return VROverlayError_InvalidParameter;
	// The path is UTF-8, as every OpenVR string
	std::string why;
	std::optional<Image> image = LoadImageFile(PathFromUtf8(pchFilePath), &why);
	if (!image) {
		OOVR_LOGF("SetOverlayFromFile: '%s' can't be shown: %s", pchFilePath, why.c_str());
		return VROverlayError_InvalidParameter;
	}
	return SetOverlayRaw(ulOverlayHandle, image->rgba.data(), image->width, image->height, 4);
}
EVROverlayError BaseOverlay::GetOverlayTexture(VROverlayHandle_t ulOverlayHandle, void** pNativeTextureHandle, void* pNativeTextureRef, uint32_t* pWidth, uint32_t* pHeight, uint32_t* pNativeFormat, ETextureType* pAPIType, EColorSpace* pColorSpace, VRTextureBounds_t* pTextureBounds)
{
	USEH();
	if (pNativeTextureHandle)
		*pNativeTextureHandle = nullptr;
	if (pWidth)
		*pWidth = 0;
	if (pHeight)
		*pHeight = 0;
	if (pNativeFormat)
		*pNativeFormat = 0;
	// The owner's own texture, as SteamVR answers the overlay's owner; an image from memory or a file has none
	if (!overlay->texture.handle)
		return VROverlayError_InvalidTexture;
	if (pNativeTextureHandle)
		*pNativeTextureHandle = overlay->texture.handle;
	if (pAPIType)
		*pAPIType = overlay->texture.eType;
	if (pColorSpace)
		*pColorSpace = overlay->texture.eColorSpace;
	if (pTextureBounds)
		*pTextureBounds = overlay->textureBounds;
	return GetOverlayTextureSize(ulOverlayHandle, pWidth, pHeight);
}
EVROverlayError BaseOverlay::ReleaseNativeOverlayHandle(VROverlayHandle_t ulOverlayHandle, void* pNativeTextureHandle)
{
	USEH();
	// GetOverlayTexture hands out the owner's own texture, which isn't ours to release
	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayTextureSize(VROverlayHandle_t ulOverlayHandle, uint32_t* pWidth, uint32_t* pHeight)
{
	USEH();
	if (pWidth)
		*pWidth = 0;
	if (pHeight)
		*pHeight = 0;
	std::lock_guard<std::recursive_mutex> lock(layersLock);
	uint32_t width = 0, height = 0;
	if (!overlay->rawImage.Empty()) {
		width = overlay->rawImage.width;
		height = overlay->rawImage.height;
	} else if (overlay->texture.handle) {
		// The swapchain holds the bounds' area of the texture
		const XrExtent2Di& extent = overlay->compositionLayer.quad.subImage.imageRect.extent;
		const float u = std::abs(overlay->textureBounds.uMax - overlay->textureBounds.uMin);
		const float v = std::abs(overlay->textureBounds.vMax - overlay->textureBounds.vMin);
		if (extent.width > 0 && extent.height > 0 && u > 0 && v > 0) {
			width = (uint32_t)std::lround(extent.width / u);
			height = (uint32_t)std::lround(std::abs(extent.height) / v);
		}
	}
	if (!width || !height)
		return VROverlayError_InvalidTexture;
	if (pWidth)
		*pWidth = width;
	if (pHeight)
		*pHeight = height;
	return VROverlayError_None;
}
EVROverlayError BaseOverlay::CreateDashboardOverlay(const char* pchOverlayKey, const char* pchOverlayFriendlyName, VROverlayHandle_t* pMainHandle, VROverlayHandle_t* pThumbnailHandle)
{
	if (pMainHandle)
		*pMainHandle = k_ulOverlayHandleInvalid;
	if (pThumbnailHandle)
		*pThumbnailHandle = k_ulOverlayHandleInvalid;
	OC_UNSUPPORTED(VROverlayError_RequestFailed, "CreateDashboardOverlay: there's no dashboard yet");
}
bool BaseOverlay::IsDashboardVisible()
{
	// TODO should this be based of whether Dash is open?
	// Probably, but handling focus opens some other issues as it triggers under other conditions.
	return false;
}
bool BaseOverlay::IsActiveDashboardOverlay(VROverlayHandle_t ulOverlayHandle)
{
	OC_UNSUPPORTED(false, "IsActiveDashboardOverlay: there's no dashboard yet");
}
EVROverlayError BaseOverlay::SetDashboardOverlaySceneProcess(VROverlayHandle_t ulOverlayHandle, uint32_t unProcessId)
{
	USEH();
	OC_UNSUPPORTED(VROverlayError_None, "SetDashboardOverlaySceneProcess: there's no dashboard yet");
}
EVROverlayError BaseOverlay::GetDashboardOverlaySceneProcess(VROverlayHandle_t ulOverlayHandle, uint32_t* punProcessId)
{
	USEH();
	if (punProcessId)
		*punProcessId = 0;
	OC_UNSUPPORTED(VROverlayError_RequestFailed, "GetDashboardOverlaySceneProcess: there's no dashboard yet");
}
void BaseOverlay::ShowDashboard(const char* pchOverlayToShow)
{
	OC_UNSUPPORTED_VOID("ShowDashboard: there's no dashboard yet");
}
TrackedDeviceIndex_t BaseOverlay::GetPrimaryDashboardDevice()
{
	OC_UNSUPPORTED(k_unTrackedDeviceIndexInvalid, "GetPrimaryDashboardDevice: there's no dashboard yet");
}
EVROverlayError BaseOverlay::ShowKeyboardWithDispatch(EGamepadTextInputMode eInputMode, EGamepadTextInputLineMode eLineInputMode,
    const char* pchDescription, uint32_t unCharMax, const char* pchExistingText, bool bUseMinimalMode, uint64_t uUserValue,
    VRKeyboard::eventDispatch_t eventDispatch)
{
	// Submit a KeyboardDone event since the actual keyboard is not implemented yet.  This allows certain games to still proceed instead of crash.
	SubmitPlaceholderKeyboardEvent(VREvent_KeyboardDone, eventDispatch, uUserValue);

#ifndef OC_XR_PORT
	if (!BaseCompositor::dxcomp)
		return VROverlayError_RequestFailed;

	if (eLineInputMode != k_EGamepadTextInputLineModeSingleLine)
		return VROverlayError_RequestFailed;

	// TODO use description
	keyboard = make_unique<VRKeyboard>(BaseCompositor::dxcomp->GetDevice(), uUserValue, unCharMax, bUseMinimalMode, eventDispatch,
	    (VRKeyboard::EGamepadTextInputMode)eInputMode);

	keyboard->contents(VRKeyboard::CHAR_CONV.from_bytes(pchExistingText));

	BaseSystem* system = GetUnsafeBaseSystem();
	if (system) {
		system->_BlockInputsUntilReleased();
	}
#endif

	return VROverlayError_None;
}

/** Placeholder method for submitting a KeyboardDone event when asked to show the keyboard since it is not implemented yet. **/
void BaseOverlay::SubmitPlaceholderKeyboardEvent(vr::EVREventType ev, VRKeyboard::eventDispatch_t eventDispatch, uint64_t userValue)
{
	VREvent_Keyboard_t data{};
	data.uUserValue = userValue;

	VREvent_t evt = { 0 };
	evt.eventType = ev;
	evt.trackedDeviceIndex = 0;
	evt.data.keyboard = data;

	eventDispatch(evt);
}

EVROverlayError BaseOverlay::ShowKeyboard(EGamepadTextInputMode eInputMode, EGamepadTextInputLineMode eLineInputMode,
    const char* pchDescription, uint32_t unCharMax, const char* pchExistingText, bool bUseMinimalMode, uint64_t uUserValue)
{

	VRKeyboard::eventDispatch_t dispatch = [](VREvent_t ev) {
		BaseSystem* sys = GetUnsafeBaseSystem();
		if (sys) {
			sys->_EnqueueEvent(ev);
		}
	};

	return ShowKeyboardWithDispatch(eInputMode, eLineInputMode, pchDescription, unCharMax, pchExistingText, bUseMinimalMode, uUserValue, dispatch);
}
EVROverlayError BaseOverlay::ShowKeyboard(EGamepadTextInputMode eInputMode, EGamepadTextInputLineMode eLineInputMode, uint32_t unFlags,
    const char* pchDescription, uint32_t unCharMax, const char* pchExistingText, uint64_t uUserValue)
{
	// KeyboardFlag_Minimal = 1 (EKeyboardFlags)
	return ShowKeyboard(eInputMode, eLineInputMode, pchDescription, unCharMax, pchExistingText, (unFlags & 1) != 0, uUserValue);
}
EVROverlayError BaseOverlay::ShowKeyboardForOverlay(VROverlayHandle_t ulOverlayHandle,
    EGamepadTextInputMode eInputMode, EGamepadTextInputLineMode eLineInputMode,
    const char* pchDescription, uint32_t unCharMax, const char* pchExistingText,
    bool bUseMinimalMode, uint64_t uUserValue)
{

	USEH();

	VRKeyboard::eventDispatch_t dispatch = [overlay](VREvent_t ev) {
		overlay->eventQueue.push(ev);
	};

	return ShowKeyboardWithDispatch(eInputMode, eLineInputMode, pchDescription, unCharMax, pchExistingText, bUseMinimalMode, uUserValue, dispatch);
}
EVROverlayError BaseOverlay::ShowKeyboardForOverlay(VROverlayHandle_t ulOverlayHandle, EGamepadTextInputMode eInputMode,
    EGamepadTextInputLineMode eLineInputMode, uint32_t unFlags, const char* pchDescription, uint32_t unCharMax,
    const char* pchExistingText, uint64_t uUserValue)
{
	// KeyboardFlag_Minimal = 1 (EKeyboardFlags)
	return ShowKeyboardForOverlay(ulOverlayHandle, eInputMode, eLineInputMode, pchDescription, unCharMax, pchExistingText, (unFlags & 1) != 0, uUserValue);
}
uint32_t BaseOverlay::GetKeyboardText(char* pchText, uint32_t cchText)
{
	string str = keyboard ? VRKeyboard::CHAR_CONV.to_bytes(keyboard->contents()) : keyboardCache;

	// Since keyboard is not functional yet, return this default text (Adventurer because this fix was made specifically for Skyrim VR)
	str = "Adventurer";

	if (!pchText || cchText == 0)
		return (uint32_t)str.size() + 1;

	// Within cchText, cut short and terminated if it doesn't fit; returns the length copied, without the terminator
	CopyOutString(str, pchText, cchText);
	return (uint32_t)strlen(pchText);
}
void BaseOverlay::HideKeyboard()
{
	// First, if the keyboard is currently open, cache it's contents
	if (keyboard) {
		keyboardCache = VRKeyboard::CHAR_CONV.to_bytes(keyboard->contents());
	}

	// Delete the keyboard instance
	{
		std::lock_guard<std::recursive_mutex> lock(layersLock);
		keyboard.reset();
	}

	BaseSystem* system = GetUnsafeBaseSystem();
	if (system) {
		system->_BlockInputsUntilReleased();
	}
}
void BaseOverlay::SetKeyboardTransformAbsolute(ETrackingUniverseOrigin eTrackingOrigin, const HmdMatrix34_t* pmatTrackingOriginToKeyboardTransform)
{
	if (!keyboard || !pmatTrackingOriginToKeyboardTransform) {
		OOVR_LOG_ONCE("SetKeyboardTransformAbsolute with no keyboard open: ignored");
		return;
	}

#ifdef OC_XR_PORT
	OC_UNSUPPORTED_VOID("SetKeyboardTransformAbsolute: the keyboard isn't drawn yet");
#else
	BaseCompositor* compositor = GetUnsafeBaseCompositor();
	if (compositor && eTrackingOrigin != compositor->GetTrackingSpace()) {
		OC_UNSUPPORTED_VOID("SetKeyboardTransformAbsolute: origin %d isn't the compositor's %d", (int)eTrackingOrigin, (int)compositor->GetTrackingSpace());
	}

	keyboard->SetTransform(*pmatTrackingOriginToKeyboardTransform);
#endif
}
void BaseOverlay::SetKeyboardPositionForOverlay(VROverlayHandle_t ulOverlayHandle, HmdRect2_t avoidRect)
{
	OC_UNSUPPORTED_VOID("SetKeyboardPositionForOverlay: the keyboard isn't drawn yet");
}
EVROverlayError BaseOverlay::SetOverlayIntersectionMask(VROverlayHandle_t ulOverlayHandle, OOVR_VROverlayIntersectionMaskPrimitive_t* pMaskPrimitives, uint32_t unNumMaskPrimitives, uint32_t unPrimitiveSize)
{
	USEH();
	if (!pMaskPrimitives || unNumMaskPrimitives == 0) {
		overlay->intersectionMask.clear(); // the whole overlay
		return VROverlayError_None;
	}
	if (unPrimitiveSize != sizeof(OOVR_VROverlayIntersectionMaskPrimitive_t))
		OC_UNSUPPORTED(VROverlayError_None, "SetOverlayIntersectionMask: primitives of %u bytes, not %u: kept as the whole overlay",
		    unPrimitiveSize, (unsigned)sizeof(OOVR_VROverlayIntersectionMaskPrimitive_t));
	// Bounded: a mask is a few shapes, not a texture's worth
	static constexpr uint32_t maxPrimitives = 4096;
	if (unNumMaskPrimitives > maxPrimitives)
		OC_UNSUPPORTED(VROverlayError_None, "SetOverlayIntersectionMask: %u primitives, more than %u: kept as the whole overlay",
		    unNumMaskPrimitives, maxPrimitives);
	overlay->intersectionMask.assign(pMaskPrimitives, pMaskPrimitives + unNumMaskPrimitives);
	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayFlags(VROverlayHandle_t ulOverlayHandle, uint32_t* pFlags)
{
	USEH();
	if (!pFlags)
		return VROverlayError_InvalidParameter;
	*pFlags = (uint32_t)overlay->flags;
	return VROverlayError_None;
}
BaseOverlay::VRMessageOverlayResponse BaseOverlay::ShowMessageOverlay(const char* pchText, const char* pchCaption, const char* pchButton0Text, const char* pchButton1Text, const char* pchButton2Text, const char* pchButton3Text)
{
	OC_UNSUPPORTED(VRMessageOverlayResponse_CouldntFindSystemOverlay, "ShowMessageOverlay: there are no message overlays yet");
}
void BaseOverlay::CloseMessageOverlay()
{
	OC_UNSUPPORTED_VOID("CloseMessageOverlay: there are no message overlays yet");
}

EVROverlayError BaseOverlay::SetOverlayPreCurvePitch(vr::VROverlayHandle_t ulOverlayHandle, float fRadians)
{
	USEH();
	overlay->preCurvePitch = fRadians;
	return VROverlayError_None;
}

EVROverlayError BaseOverlay::GetOverlayPreCurvePitch(vr::VROverlayHandle_t ulOverlayHandle, float* pfRadians)
{
	USEH();
	if (!pfRadians)
		return VROverlayError_InvalidParameter;
	*pfRadians = overlay->preCurvePitch;
	return VROverlayError_None;
}

EVROverlayError BaseOverlay::WaitFrameSync(uint32_t nTimeoutMs)
{
	// Until the next frame starts, the game's or OpenLamina's own; before any has, there's nothing to wait for
	if (!FrameStats::Get().AnyFrame())
		return VROverlayError_None;
	return FrameStats::Get().WaitForFrame(std::chrono::milliseconds(nTimeoutMs)) ? VROverlayError_None : VROverlayError_TimedOut;
}

EVROverlayError BaseOverlay::CreateSubviewOverlay(VROverlayHandle_t parentOverlayHandle, const char* pchSubviewOverlayKey,
    const char* pchSubviewOverlayName, VROverlayHandle_t* pSubviewOverlayHandle)
{
	if (pSubviewOverlayHandle)
		*pSubviewOverlayHandle = k_ulOverlayHandleInvalid;
	return VROverlayError_RequestFailed;
}

EVROverlayError BaseOverlay::SetSubviewPosition(VROverlayHandle_t ulOverlayHandle, float fX, float fY)
{
	return VROverlayError_RequestFailed;
}
