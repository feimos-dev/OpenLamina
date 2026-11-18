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
#include "Misc/Canvas.h"
#include "Misc/Keyboard/KeyboardLayout.h"
#include "Misc/LaserMouse.h"
#include "Misc/OverlayMaths.h"
#include "Misc/ScopeGuard.h"
#include "BaseInput.h"
#include "convert.h"
#include "generated/static_bases.gen.h"
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
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
	VROverlayInputMethod inputMethod = VROverlayInputMethod_None; // Mouse: the lasers send it mouse events
	// HandleControllerOverlayInteractionAsMouse: a pointer target for this many more of the lasers' updates
	int asMouseUpdates = 0;
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

// Where each cursor overlay goes: the point where a laser meets the overlay it's the cursor of (SetOverlayCursor), set
// by _UpdatePointer. Read while overlays are placed, which some calls do without the layers' lock.
static std::mutex cursorPointsLock;
static std::map<const void*, XrPosef> cursorPoints;

// Where a cursor overlay's laser pointer meets the overlay it points at; nothing while nothing points at one
static std::optional<XrPosef> CursorPoint(const void* cursorOverlay)
{
	std::lock_guard<std::mutex> lock(cursorPointsLock);
	auto it = cursorPoints.find(cursorOverlay);
	if (it == cursorPoints.end())
		return {};
	return it->second;
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
		if (std::optional<XrPosef> point = CursorPoint(this)) {
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

namespace {

// The cursor: a circle on a 2 cm quad
constexpr int kCursorPixels = 64;
constexpr float kCursorMetres = 0.02f;

// A message box: a panel a millimetre per pixel, its buttons in a row along the bottom
constexpr int kPanelWidth = 1000, kPanelHeight = 500;
constexpr float kPanelMetresPerPixel = 0.001f;
constexpr int kPanelMargin = 40, kButtonGap = 20, kButtonHeight = 70, kButtonBottom = 40;
constexpr float kPanelDistance = 1.5f;

// The keyboard: ahead of the head and below it
constexpr float kKeyboardDistance = 1.0f, kKeyboardBelow = 0.4f;

double NowSeconds()
{
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// A quad of OpenLamina's own, in the frame's space, from a swapchain filled from memory
XrCompositionLayerQuad OwnQuad(const CpuSwapchain& chain, const XrPosef& pose, float width, float height)
{
	XrCompositionLayerQuad quad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
	quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
	quad.space = xr_space_from_ref_space_type(BaseSystem::currentSpace);
	quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	quad.subImage = chain.SubImage();
	quad.pose = pose;
	quad.size = { width, height };
	return quad;
}

// A pose ahead of the head and below it, level (turned only about the vertical) and facing the head: where the
// keyboard and a message box go. Without a head pose, from the origin looking along -Z.
XrPosef AheadOfHead(vr::ETrackingUniverseOrigin space, float ahead, float below)
{
	glm::vec3 head(0.0f), forward(0.0f, 0.0f, -1.0f);
	if (std::shared_ptr<ITrackedDevice> hmd = BackendManager::Instance().GetDevice(vr::k_unTrackedDeviceIndex_Hmd)) {
		TrackedDevicePose_t pose{};
		hmd->GetPose(space, &pose, ETrackingStateType::TrackingStateType_Rendering);
		if (pose.bPoseIsValid) {
			const glm::mat4 m = S2G_m34(pose.mDeviceToAbsoluteTracking);
			head = glm::vec3(m[3]);
			glm::vec3 f = -glm::vec3(m[2]);
			f.y = 0;
			if (glm::length(f) > 1e-3f)
				forward = glm::normalize(f);
		}
	}
	const glm::quat turn = glm::angleAxis(std::atan2(-forward.x, -forward.z), glm::vec3(0.0f, 1.0f, 0.0f));
	const glm::vec3 at = head + forward * ahead - glm::vec3(0.0f, below, 0.0f);
	return XrPosef{ G2X_quat(turn), G2X_v3f(at) };
}

// Each hand's laser, from its aim pose in the frame's space; none while input isn't available
std::array<PointerRay, 2> ReadRays(bool available)
{
	std::array<PointerRay, 2> rays{};
	rays[0].hand = 0;
	rays[1].hand = 1;
	BaseInput* input = GetUnsafeBaseInput();
	if (!available || !input || !xr_gbl)
		return rays;
	const XrSpace base = xr_space_from_ref_space_type(BaseSystem::currentSpace);
	const XrTime time = xr_gbl->GetBestTime();
	IBackend* backend = BackendManager::Instance().GetBackendInstance();
	for (int h = 0; h < 2; h++) {
		BaseInput::HandPointer hand{};
		if (!input->GetHandPointer(h, base, time, hand))
			continue;
		PointerRay& ray = rays[h];
		ray.origin = hand.aim.position;
		ray.dir = G2X_v3f(X2G_quat(hand.aim.orientation) * glm::vec3(0.0f, 0.0f, -1.0f));
		ray.click = hand.trigger >= 0.5f;
		ray.scrollX = hand.stickX;
		ray.scrollY = hand.stickY;
		ray.valid = true;
		if (backend) {
			if (std::shared_ptr<ITrackedDevice> dev = backend->GetDeviceByHand((ITrackedDevice::TrackedDeviceType)h))
				ray.device = dev->DeviceIndex();
		}
	}
	return rays;
}

// UTF-8 as the font draws it (characters it can't decode as '?')
std::wstring WideText(const char* utf8)
{
	if (!utf8)
		return {};
	try {
		return VRKeyboard::CHAR_CONV.from_bytes(utf8);
	} catch (const std::range_error&) {
		std::wstring out;
		for (const char* c = utf8; *c; c++)
			out += (unsigned char)*c < 0x80 ? (wchar_t)*c : L'?';
		return out;
	}
}

// Text broken into lines no wider than width, at spaces and newlines (a word wider than a line, at a character)
std::vector<std::wstring> WrapText(const Canvas& canvas, const std::wstring& text, int width)
{
	std::vector<std::wstring> lines;
	std::wstring line, word;
	auto endWord = [&] {
		if (word.empty())
			return;
		std::wstring joined = line.empty() ? word : line + L" " + word;
		if (canvas.TextWidth(joined) <= width) {
			line = joined;
		} else {
			if (!line.empty())
				lines.push_back(line);
			line.clear();
			for (wchar_t c : word) {
				if (!line.empty() && canvas.TextWidth(line + c) > width) {
					lines.push_back(line);
					line.clear();
				}
				line += c;
			}
		}
		word.clear();
	};
	for (wchar_t c : text) {
		if (c == L' ' || c == L'\t') {
			endWord();
		} else if (c == L'\n') {
			endWord();
			lines.push_back(line);
			line.clear();
		} else if (c != L'\r') {
			word += c;
		}
	}
	endWord();
	if (!line.empty())
		lines.push_back(line);
	return lines;
}

struct PanelRect {
	int x, y, w, h;
	bool Contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

// Button i of n, along the panel's bottom
PanelRect ButtonRect(size_t i, size_t n)
{
	const int count = (int)std::max<size_t>(n, 1);
	const int w = (kPanelWidth - 2 * kPanelMargin - (count - 1) * kButtonGap) / count;
	return PanelRect{ kPanelMargin + (int)i * (w + kButtonGap), kPanelHeight - kButtonBottom - kButtonHeight, w, kButtonHeight };
}

} // namespace

class BaseOverlay::Pointing {
public:
	LaserMouse mouse;
	bool running = false; // the lasers ran at the last update

	// The cursor quad, where the laser driving the mouse meets a target without a cursor overlay of its own
	std::unique_ptr<CpuSwapchain> cursorChain;
	bool cursorUnavailable = false; // no swapchain could be made for it
	bool cursorShown = false;
	XrPosef cursorPose{ { 0, 0, 0, 1 }, { 0, 0, 0 } };

	// The keyboard's quad; its placement in keyboardOrigin, and in the frame's space as last drawn
	std::unique_ptr<CpuSwapchain> keyboardChain;
	bool keyboardShown = false;
	XrPosef keyboardPose{ { 0, 0, 0, 1 }, { 0, 0, 0 } };
	vr::ETrackingUniverseOrigin keyboardOrigin = vr::TrackingUniverseStanding;
	vr::VROverlayHandle_t keyboardFor = vr::k_ulOverlayHandleInvalid;
	bool keyboardModal = false; // only the keyboard can be pointed at (KeyboardFlag_Modal)

	// A message box (ShowMessageOverlay), while one is shown
	struct Message {
		std::wstring caption, text;
		std::vector<std::pair<int, std::wstring>> buttons; // the answer each gives, its label
		overlay_maths::Placement placement{};
		std::unique_ptr<CpuSwapchain> chain;
		Image image;
		bool dirty = true; // to be drawn again
		bool uploaded = false; // the image is in the swapchain
		int hovered = -1, pressed = -1; // buttons by their place
		LaserMouse mouse; // its own: the other overlays get no events while it's up
		vr::VROverlayHandle_t Handle() const { return (vr::VROverlayHandle_t)(uintptr_t)this; }

		// The panel as it looks now: the caption, the text and the buttons
		void Draw()
		{
			Canvas canvas(kPanelWidth, kPanelHeight);
			canvas.FillRect(0, 0, kPanelWidth, kPanelHeight, 0x2b2f36f0, 24);
			const int line = Canvas::LineHeight();
			int y = 30;
			canvas.Text(kPanelMargin, y, caption, 0xffffffff);
			y += line + 8;
			canvas.FillRect(kPanelMargin, y, kPanelWidth - 2 * kPanelMargin, 2, 0x6a7486ff);
			y += 16;
			const int textEnd = ButtonRect(0, buttons.size()).y - 16;
			for (const std::wstring& row : WrapText(canvas, text, kPanelWidth - 2 * kPanelMargin)) {
				if (y + line > textEnd)
					break;
				canvas.Text(kPanelMargin, y, row, 0xd8d8d8ff);
				y += line + 4;
			}
			for (size_t i = 0; i < buttons.size(); i++) {
				const PanelRect r = ButtonRect(i, buttons.size());
				const uint32_t colour = (int)i == pressed ? 0x2a5080ff : ((int)i == hovered ? 0x5a86c0ff : 0x3a6ea5ff);
				canvas.FillRect(r.x, r.y, r.w, r.h, colour, 10);
				const std::wstring& label = buttons[i].second;
				canvas.Text(r.x + (r.w - canvas.TextWidth(label)) / 2, r.y + (r.h - line) / 2, label, 0xffffffff);
			}
			image = canvas.Pixels();
			dirty = false;
			uploaded = false;
		}
	};
	std::unique_ptr<Message> message;
	std::atomic<bool> closeMessage{ false };

	// The layers built from them for a frame, valid until it has ended
	XrCompositionLayerQuad keyboardLayer{}, messageLayer{}, cursorLayer{};

	// The cursor shown at a point on a target, facing out of it: its swapchain made and filled on the game's thread
	void ShowCursor(const XrPosef& point)
	{
		if (!cursorChain && !cursorUnavailable) {
			cursorChain = CpuSwapchain::Create(kCursorPixels, kCursorPixels);
			cursorUnavailable = !cursorChain;
		}
		if (!cursorChain)
			return;
		if (!cursorChain->Live()) {
			Canvas canvas(kCursorPixels, kCursorPixels);
			canvas.FillRect(0, 0, kCursorPixels, kCursorPixels, 0x202020c0, kCursorPixels / 2);
			canvas.FillRect(8, 8, kCursorPixels - 16, kCursorPixels - 16, 0xffffffff, kCursorPixels / 2 - 8);
			cursorChain->Upload(canvas.Pixels());
		}
		// A little in front of the target, so it isn't hidden in it
		const glm::quat q = X2G_quat(point.orientation);
		cursorPose = XrPosef{ point.orientation, G2X_v3f(X2G_v3f(point.position) + q * glm::vec3(0.0f, 0.0f, 0.002f)) };
		cursorShown = cursorChain->Live();
	}
};

BaseOverlay::BaseOverlay()
    : pointing(std::make_unique<Pointing>())
{
}

void BaseOverlay::_UpdatePointer()
{
	// The session lock before the layers' lock, as everywhere: swapchains are filled and input read under it
	auto session = xr_session.lock_shared();
	auto* backend = (XrBackend*)BackendManager::Instance().GetBackendInstance();

	// Cheap first: is there anything to point at?
	bool wanted = false;
	{
		std::lock_guard<std::recursive_mutex> lock(layersLock);
		wanted = keyboard != nullptr;
		for (const auto& kv : overlays) {
			const OverlayData* o = kv.second;
			if (o && o->visible && (o->inputMethod == VROverlayInputMethod_Mouse || o->asMouseUpdates > 0)
			    && (o->texture.handle != nullptr || !o->rawImage.Empty())) {
				wanted = true;
				break;
			}
		}
		if (!wanted && !pointing->running)
			return;
	}
	const bool focused = backend && backend->IsInputAvailable();
	// Outside the layers' lock: reading input takes the input's own lock
	const std::array<PointerRay, 2> rays = ReadRays(wanted && focused);
	BaseCompositor* compositor = GetUnsafeBaseCompositor();
	const vr::ETrackingUniverseOrigin space = compositor ? compositor->GetTrackingSpace() : vr::TrackingUniverseStanding;

	std::unique_ptr<CpuSwapchain> dropped; // a closed keyboard's, destroyed once the lock is let go
	bool keyboardClosed = false;
	{
		std::lock_guard<std::recursive_mutex> lock(layersLock);
		Pointing& p = *pointing;

		if (keyboard) {
			XrPosef pose = keyboard->Placement();
			if (p.keyboardOrigin != space)
				pose = overlay_maths::ChangeOrigin(pose, p.keyboardOrigin, space, SeatedInStanding());
			p.keyboardPose = pose;
		}

		std::vector<PointerTarget> targets;
		if (wanted && focused) {
			if (keyboard) {
				PointerTarget t{};
				t.handle = keyboard->Handle();
				t.placement = overlay_maths::Placement{ p.keyboardPose, keyboard->WidthMeters(), keyboard->HeightMeters(), 0 };
				t.mouseScale = { { (float)keyboard->CanvasWidth(), (float)keyboard->CanvasHeight() } };
				t.sortOrder = INT_MAX; // on top of every overlay
				targets.push_back(t);
			}
			if (!(keyboard && p.keyboardModal)) {
				for (const auto& kv : overlays) {
					OverlayData* o = kv.second;
					if (!o || !o->visible || !(o->inputMethod == VROverlayInputMethod_Mouse || o->asMouseUpdates > 0))
						continue;
					if (o->texture.handle == nullptr && o->rawImage.Empty())
						continue;
					PointerTarget t{};
					if (!o->ComputePlacement(space, validOverlays, t.placement))
						continue;
					t.handle = (VROverlayHandle_t)(uintptr_t)o;
					t.mask = o->intersectionMask;
					t.mouseScale = o->mouseScale;
					t.flags = (uint32_t)o->flags;
					t.sortOrder = (int)std::min<uint32_t>(o->sortOrder, INT_MAX - 1);
					targets.push_back(std::move(t));
				}
			}
		}
		for (const auto& kv : overlays) {
			if (kv.second && kv.second->asMouseUpdates > 0)
				kv.second->asMouseUpdates--;
		}

		// Stopped (nothing to point at, or the session lost focus): the update without rays lets go of a click and
		// the focus
		std::vector<PointerEvent> events;
		p.mouse.Update(rays, targets, NowSeconds(), events);
		p.running = wanted && focused;
		for (const PointerEvent& e : events) {
			if (keyboard && e.handle == keyboard->Handle())
				continue;
			OverlayData* o = (OverlayData*)(uintptr_t)e.handle;
			if (validOverlays.count(o))
				o->eventQueue.push(e.event);
		}

		if (keyboard) {
			keyboard->OnPointer(p.mouse, events);
			for (const VREvent_t& e : keyboard->TakeEvents())
				DispatchKeyboardEvent(e, p.keyboardFor);
			if (keyboard->Closed()) {
				dropped = DropKeyboard();
				keyboardClosed = true;
			}
		}
		if (keyboard && p.keyboardChain) {
			if (keyboard->Dirty() || !p.keyboardChain->Live()) {
				Canvas canvas(keyboard->CanvasWidth(), keyboard->CanvasHeight());
				keyboard->Draw(canvas);
				if (p.keyboardChain->Upload(canvas.Pixels()))
					keyboard->MarkDrawn();
			}
			p.keyboardShown = p.keyboardChain->Live();
		}

		// The cursor: a cursor overlay the target has, else OpenLamina's own (unless the target hides it)
		{
			std::lock_guard<std::mutex> cursors(cursorPointsLock);
			cursorPoints.clear();
		}
		p.cursorShown = false;
		const int hand = p.mouse.ActiveHand();
		const std::optional<overlay_maths::Hit> hit = p.mouse.Cursor(hand);
		const VROverlayHandle_t target = p.mouse.Target(hand);
		const PointerTarget* on = nullptr;
		for (const PointerTarget& t : targets)
			if (t.handle == target)
				on = &t;
		if (p.running && hit && on) {
			const XrPosef point = overlay_maths::PointOn(on->placement, hit->u, hit->v);
			OverlayData* o = validOverlays.count((OverlayData*)(uintptr_t)target) ? (OverlayData*)(uintptr_t)target : nullptr;
			OverlayData* cursor = o && validOverlays.count((OverlayData*)(uintptr_t)o->cursor) ? (OverlayData*)(uintptr_t)o->cursor : nullptr;
			if (cursor && cursor->visible) {
				std::lock_guard<std::mutex> cursors(cursorPointsLock);
				cursorPoints[cursor] = point;
			} else if (!(on->flags & kOverlayFlagHideLaserIntersection)) {
				p.ShowCursor(point);
			}
		}
	}
	dropped.reset();
	if (keyboardClosed) {
		if (BaseSystem* system = GetUnsafeBaseSystem())
			system->_BlockInputsUntilReleased();
	}
}

bool BaseOverlay::_AnyVisible()
{
	std::lock_guard<std::recursive_mutex> lock(layersLock);
	if (keyboard || pointing->message)
		return true;
	for (const auto& kv : overlays) {
		if (kv.second && kv.second->visible && (kv.second->texture.handle != nullptr || !kv.second->rawImage.Empty()))
			return true;
	}
	return false;
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

	// The keyboard takes the controllers' input from the game while it's shown (_HandleOverlayInput)
	if (keyboard)
		checkUsingInput = true;

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

	// OpenLamina's own quads on top of the overlays: the keyboard, a message box, then the cursor. Filled on the game's
	// thread (_UpdatePointer, ShowMessageOverlay): only those already in a swapchain are drawn.
	Pointing& p = *pointing;
	if (keyboard && p.keyboardShown && p.keyboardChain && p.keyboardChain->Live()) {
		p.keyboardLayer = OwnQuad(*p.keyboardChain, p.keyboardPose, keyboard->WidthMeters(), keyboard->HeightMeters());
		layerHeaders.push_back((XrCompositionLayerBaseHeader*)&p.keyboardLayer);
	}
	if (p.message && p.message->chain && p.message->chain->Live()) {
		const overlay_maths::Placement& at = p.message->placement;
		p.messageLayer = OwnQuad(*p.message->chain, at.pose, at.width, at.height);
		layerHeaders.push_back((XrCompositionLayerBaseHeader*)&p.messageLayer);
	}
	if (p.cursorShown && p.cursorChain && p.cursorChain->Live()) {
		p.cursorLayer = OwnQuad(*p.cursorChain, p.cursorPose, kCursorMetres, kCursorMetres);
		layerHeaders.push_back((XrCompositionLayerBaseHeader*)&p.cursorLayer);
	}

	usingInput = checkUsingInput;
	layers = layerHeaders.data();
	return static_cast<int>(layerHeaders.size());
}

bool BaseOverlay::_HandleOverlayInput(EVREye side, TrackedDeviceIndex_t index, VRControllerState_t state)
{
	// The keyboard is typed on with the lasers (_UpdatePointer): while it's shown, the game doesn't get the input
	if (!usingInput)
		return true;

	return !keyboard;
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

	// The lasers and the keyboard queue events on the game's thread
	std::lock_guard<std::recursive_mutex> lock(layersLock);
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
	USEHB();
	// The lasers point at it as at an overlay that takes mouse input, for as long as it's asked (each frame)
	std::lock_guard<std::recursive_mutex> lock(layersLock);
	overlay->asMouseUpdates = 2;
	std::shared_ptr<ITrackedDevice> dev = BackendManager::Instance().GetDevice(unControllerDeviceIndex);
	if (!dev || (dev->GetHand() != ITrackedDevice::HAND_LEFT && dev->GetHand() != ITrackedDevice::HAND_RIGHT))
		return false;
	const int hand = dev->GetHand() == ITrackedDevice::HAND_LEFT ? 0 : 1;
	return pointing->mouse.Target(hand) == ulOverlayHandle;
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
	USEHB();
	// The overlay the laser driving the mouse is on
	std::lock_guard<std::recursive_mutex> lock(layersLock);
	return pointing->mouse.Focus() == ulOverlayHandle;
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
	// The hand whose laser is on the overlay; none is: nothing to pulse
	int hand;
	{
		std::lock_guard<std::recursive_mutex> lock(layersLock);
		hand = pointing->mouse.HandOn(ulOverlayHandle);
	}
	if (hand < 0)
		return VROverlayError_None;
	if (BaseInput* input = GetUnsafeBaseInput())
		input->TriggerHandHaptic(hand, fDurationSeconds, fFrequency, fAmplitude);
	return VROverlayError_None;
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
    uint32_t unFlags, const char* pchDescription, uint32_t unCharMax, const char* pchExistingText, uint64_t uUserValue,
    VROverlayHandle_t forOverlay)
{
	const std::string existing = pchExistingText ? pchExistingText : "";
	VRKeyboard::Mode mode = VRKeyboard::Mode::Normal;
	if (unFlags & VRKeyboard::kFlagMinimal)
		mode = VRKeyboard::Mode::Minimal;
	else if (eInputMode == k_EGamepadTextInputModePassword)
		mode = VRKeyboard::Mode::Password;
	else if (eInputMode == k_EGamepadTextInputModeSubmit)
		mode = VRKeyboard::Mode::Submit;
	const bool multiline = eLineInputMode == k_EGamepadTextInputLineModeMultipleLines;
	// EKeyboardFlags' KeyboardFlag_Modal: nothing but the keyboard can be pointed at
	constexpr uint32_t modalFlag = 2;

	// The session lock before the layers' lock, as everywhere
	auto session = xr_session.lock_shared();
	std::unique_ptr<VRKeyboard> made;
	std::unique_ptr<CpuSwapchain> chain;
	if (KeyboardLayout::Current()) {
		made = std::make_unique<VRKeyboard>(mode, multiline, unFlags, existing, unCharMax, uUserValue);
		chain = CpuSwapchain::Create(made->CanvasWidth(), made->CanvasHeight());
	}
	std::unique_ptr<CpuSwapchain> old;
	if (!made || !chain) {
		// It can't be shown (no layout built in, or no swapchain from memory on this session): done at once, with the
		// text as given
		{
			std::lock_guard<std::recursive_mutex> lock(layersLock);
			old = DropKeyboard();
			keyboardCache = existing;
		}
		old.reset();
		SubmitPlaceholderKeyboardEvent(VREvent_KeyboardDone, forOverlay, uUserValue);
		OC_UNSUPPORTED(VROverlayError_None, "ShowKeyboard: the keyboard can't be shown on this session; answered as done, with the text as given");
	}

	BaseCompositor* compositor = GetUnsafeBaseCompositor();
	const ETrackingUniverseOrigin space = compositor ? compositor->GetTrackingSpace() : TrackingUniverseStanding;
	made->SetPlacement(AheadOfHead(space, kKeyboardDistance, kKeyboardBelow));
	{
		std::lock_guard<std::recursive_mutex> lock(layersLock);
		// One keyboard at a time: a new one replaces the one shown
		old = DropKeyboard();
		keyboard = std::move(made);
		Pointing& p = *pointing;
		p.keyboardChain = std::move(chain);
		p.keyboardShown = false;
		p.keyboardFor = forOverlay;
		p.keyboardOrigin = space;
		p.keyboardPose = keyboard->Placement();
		p.keyboardModal = (unFlags & modalFlag) != 0;
	}
	old.reset();

	BaseSystem* system = GetUnsafeBaseSystem();
	if (system)
		system->_BlockInputsUntilReleased();
	return VROverlayError_None;
}

std::unique_ptr<CpuSwapchain> BaseOverlay::DropKeyboard()
{
	if (keyboard)
		keyboardCache = keyboard->Text();
	keyboard.reset();
	pointing->keyboardShown = false;
	pointing->keyboardFor = k_ulOverlayHandleInvalid;
	return std::move(pointing->keyboardChain);
}

void BaseOverlay::DispatchKeyboardEvent(VREvent_t event, VROverlayHandle_t forOverlay)
{
	event.data.keyboard.overlayHandle = forOverlay;
	if (forOverlay != k_ulOverlayHandleInvalid) {
		std::lock_guard<std::recursive_mutex> lock(layersLock);
		OverlayData* overlay = (OverlayData*)(uintptr_t)forOverlay;
		if (validOverlays.count(overlay))
			overlay->eventQueue.push(event);
		return;
	}
	if (BaseSystem* sys = GetUnsafeBaseSystem())
		sys->_EnqueueEvent(event);
}

void BaseOverlay::SubmitPlaceholderKeyboardEvent(vr::EVREventType ev, VROverlayHandle_t forOverlay, uint64_t userValue)
{
	VREvent_t evt{};
	evt.eventType = ev;
	evt.trackedDeviceIndex = 0;
	evt.data.keyboard.uUserValue = userValue;
	DispatchKeyboardEvent(evt, forOverlay);
}

EVROverlayError BaseOverlay::ShowKeyboard(EGamepadTextInputMode eInputMode, EGamepadTextInputLineMode eLineInputMode,
    const char* pchDescription, uint32_t unCharMax, const char* pchExistingText, bool bUseMinimalMode, uint64_t uUserValue)
{
	return ShowKeyboardWithDispatch(eInputMode, eLineInputMode, bUseMinimalMode ? VRKeyboard::kFlagMinimal : 0, pchDescription, unCharMax,
	    pchExistingText, uUserValue, k_ulOverlayHandleInvalid);
}
EVROverlayError BaseOverlay::ShowKeyboard(EGamepadTextInputMode eInputMode, EGamepadTextInputLineMode eLineInputMode, uint32_t unFlags,
    const char* pchDescription, uint32_t unCharMax, const char* pchExistingText, uint64_t uUserValue)
{
	return ShowKeyboardWithDispatch(eInputMode, eLineInputMode, unFlags, pchDescription, unCharMax, pchExistingText, uUserValue,
	    k_ulOverlayHandleInvalid);
}
EVROverlayError BaseOverlay::ShowKeyboardForOverlay(VROverlayHandle_t ulOverlayHandle,
    EGamepadTextInputMode eInputMode, EGamepadTextInputLineMode eLineInputMode,
    const char* pchDescription, uint32_t unCharMax, const char* pchExistingText,
    bool bUseMinimalMode, uint64_t uUserValue)
{
	USEH();
	return ShowKeyboardWithDispatch(eInputMode, eLineInputMode, bUseMinimalMode ? VRKeyboard::kFlagMinimal : 0, pchDescription, unCharMax,
	    pchExistingText, uUserValue, ulOverlayHandle);
}
EVROverlayError BaseOverlay::ShowKeyboardForOverlay(VROverlayHandle_t ulOverlayHandle, EGamepadTextInputMode eInputMode,
    EGamepadTextInputLineMode eLineInputMode, uint32_t unFlags, const char* pchDescription, uint32_t unCharMax,
    const char* pchExistingText, uint64_t uUserValue)
{
	USEH();
	return ShowKeyboardWithDispatch(eInputMode, eLineInputMode, unFlags, pchDescription, unCharMax, pchExistingText, uUserValue, ulOverlayHandle);
}
uint32_t BaseOverlay::GetKeyboardText(char* pchText, uint32_t cchText)
{
	// The keyboard's text, or the last one's after it has gone
	std::string str;
	{
		std::lock_guard<std::recursive_mutex> lock(layersLock);
		str = keyboard ? keyboard->Text() : keyboardCache;
	}

	if (!pchText || cchText == 0)
		return (uint32_t)str.size() + 1;

	// Within cchText, cut short and terminated if it doesn't fit; returns the length copied, without the terminator
	CopyOutString(str, pchText, cchText);
	return (uint32_t)strlen(pchText);
}
void BaseOverlay::HideKeyboard()
{
	// The session lock before the layers' lock: the keyboard's swapchain goes with it
	auto session = xr_session.lock_shared();
	std::unique_ptr<CpuSwapchain> chain;
	{
		std::lock_guard<std::recursive_mutex> lock(layersLock);
		chain = DropKeyboard();
	}
	chain.reset();

	BaseSystem* system = GetUnsafeBaseSystem();
	if (system) {
		system->_BlockInputsUntilReleased();
	}
}
void BaseOverlay::SetKeyboardTransformAbsolute(ETrackingUniverseOrigin eTrackingOrigin, const HmdMatrix34_t* pmatTrackingOriginToKeyboardTransform)
{
	std::lock_guard<std::recursive_mutex> lock(layersLock);
	if (!keyboard || !pmatTrackingOriginToKeyboardTransform) {
		OOVR_LOG_ONCE("SetKeyboardTransformAbsolute with no keyboard open: ignored");
		return;
	}
	// The keyboard's centre, in that origin (moved into the frame's when drawn)
	const glm::mat4 m = S2G_m34(*pmatTrackingOriginToKeyboardTransform);
	const glm::vec3 position(m[3]);
	if (!IsFinite(position))
		OC_UNSUPPORTED_VOID("SetKeyboardTransformAbsolute: a transform that isn't finite: ignored");
	const glm::quat orientation = glm::normalize(glm::quat_cast(glm::mat3(m)));
	keyboard->SetPlacement(XrPosef{ G2X_quat(orientation), G2X_v3f(position) });
	pointing->keyboardOrigin = eTrackingOrigin;
}
void BaseOverlay::SetKeyboardPositionForOverlay(VROverlayHandle_t ulOverlayHandle, HmdRect2_t avoidRect)
{
	std::lock_guard<std::recursive_mutex> lock(layersLock);
	OverlayData* overlay = (OverlayData*)ulOverlayHandle;
	if (!overlay || !validOverlays.count(overlay) || !keyboard) {
		OOVR_LOG_ONCE("SetKeyboardPositionForOverlay with no keyboard open or no such overlay: ignored");
		return;
	}
	BaseCompositor* compositor = GetUnsafeBaseCompositor();
	const ETrackingUniverseOrigin space = compositor ? compositor->GetTrackingSpace() : TrackingUniverseStanding;
	overlay_maths::Placement placement;
	if (!overlay->ComputePlacement(space, validOverlays, placement))
		OC_UNSUPPORTED_VOID("SetKeyboardPositionForOverlay: overlay '%s' can't be placed: the keyboard stays where it is", overlay->key.c_str());
	// The rectangle is in the overlay's coordinates from its lower left: 0 to 1, or its mouse units where it's larger
	float scaleX = 1, scaleY = 1;
	if (std::max(std::abs(avoidRect.vTopLeft.v[0]), std::abs(avoidRect.vBottomRight.v[0])) > 1.0f && overlay->mouseScale.v[0] > 0)
		scaleX = overlay->mouseScale.v[0];
	if (std::max(std::abs(avoidRect.vTopLeft.v[1]), std::abs(avoidRect.vBottomRight.v[1])) > 1.0f && overlay->mouseScale.v[1] > 0)
		scaleY = overlay->mouseScale.v[1];
	const float centreU = (avoidRect.vTopLeft.v[0] + avoidRect.vBottomRight.v[0]) / 2 / scaleX;
	const float bottomV = std::min(avoidRect.vTopLeft.v[1], avoidRect.vBottomRight.v[1]) / scaleY;
	// Just below the rectangle, a little in front of the overlay
	const glm::vec3 local((centreU - 0.5f) * placement.width, (bottomV - 0.5f) * placement.height - keyboard->HeightMeters() / 2 - 0.02f, 0.05f);
	const glm::vec3 position = X2G_v3f(placement.pose.position) + X2G_quat(placement.pose.orientation) * local;
	if (!IsFinite(position))
		OC_UNSUPPORTED_VOID("SetKeyboardPositionForOverlay: no finite place below the rectangle: the keyboard stays where it is");
	keyboard->SetPlacement(XrPosef{ placement.pose.orientation, G2X_v3f(position) });
	pointing->keyboardOrigin = space;
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
	// Shown in frames of OpenLamina's own, run here until a button is clicked: without them, it can't be
	auto* backend = (XrBackend*)BackendManager::Instance().GetBackendInstance();
	if (!backend || !backend->HasOwnFrames() || !backend->sessionActive)
		OC_UNSUPPORTED(VRMessageOverlayResponse_CouldntFindSystemOverlay, "ShowMessageOverlay: no frames of OpenLamina's own to show it in");
	// Answered with the controllers' lasers, which read the game's input actions: before it has any, nothing could
	// answer it
	BaseInput* input = GetUnsafeBaseInput();
	if (!input || !input->AreActionsLoaded())
		OC_UNSUPPORTED(VRMessageOverlayResponse_CouldntFindSystemOverlay, "ShowMessageOverlay: the game has loaded no input yet, so nothing could answer it");

	auto message = std::make_unique<Pointing::Message>();
	message->caption = WideText(pchCaption);
	message->text = WideText(pchText);
	const char* labels[4] = { pchButton0Text, pchButton1Text, pchButton2Text, pchButton3Text };
	for (int i = 0; i < 4; i++) {
		if (labels[i] && *labels[i])
			message->buttons.emplace_back(i, WideText(labels[i]));
	}
	// Something to click: with no buttons, one that answers as the first
	if (message->buttons.empty())
		message->buttons.emplace_back(0, L"OK");
	message->Draw();

	// Held until it's answered: the session isn't restarted under it, nor its own frames stopped
	auto session = xr_session.lock_shared();
	{
		std::lock_guard<std::recursive_mutex> lock(layersLock);
		if (pointing->message)
			OC_UNSUPPORTED(VRMessageOverlayResponse_CouldntFindOrCreateClientOverlay, "ShowMessageOverlay: a message box is already shown");
	}
	message->chain = CpuSwapchain::Create(kPanelWidth, kPanelHeight);
	if (!message->chain)
		OC_UNSUPPORTED(VRMessageOverlayResponse_CouldntFindSystemOverlay, "ShowMessageOverlay: no swapchain from memory on this session to show it with");
	BaseCompositor* compositor = GetUnsafeBaseCompositor();
	const ETrackingUniverseOrigin space = compositor ? compositor->GetTrackingSpace() : TrackingUniverseStanding;
	message->placement = overlay_maths::Placement{ AheadOfHead(space, kPanelDistance, 0), kPanelWidth * kPanelMetresPerPixel,
		kPanelHeight * kPanelMetresPerPixel, 0 };
	pointing->closeMessage = false;
	{
		std::lock_guard<std::recursive_mutex> lock(layersLock);
		pointing->message = std::move(message);
		pointing->cursorShown = false;
	}

	// The game's frame, if it's in the middle of one, ends here (without its eye textures) so that frames can run
	backend->EndOpenAppFrame();

	int answer = -1;
	const bool ran = backend->RunOwnFramesWhile([&]() -> bool {
		if (pointing->closeMessage || backend->IsSessionLost()) {
			answer = VRMessageOverlayResponse_ApplicationQuit;
			return false;
		}
		// The game is blocked here and syncs no input: the lasers' actions are synced for it
		const bool focused = backend->IsInputAvailable();
		if (input && focused)
			input->SyncPointerActions();
		const std::array<PointerRay, 2> rays = ReadRays(focused);

		std::lock_guard<std::recursive_mutex> lock(layersLock);
		Pointing& p = *pointing;
		Pointing::Message& m = *p.message;
		PointerTarget panel{};
		panel.handle = m.Handle();
		panel.placement = m.placement;
		panel.mouseScale = { { (float)kPanelWidth, (float)kPanelHeight } };
		std::vector<PointerEvent> events;
		m.mouse.Update(rays, { panel }, NowSeconds(), events);
		auto buttonAt = [&](const VREvent_t& e) {
			for (size_t i = 0; i < m.buttons.size(); i++)
				if (ButtonRect(i, m.buttons.size()).Contains(e.data.mouse.x, e.data.mouse.y))
					return (int)i;
			return -1;
		};
		for (const PointerEvent& e : events) {
			const int at = e.event.eventType == VREvent_FocusLeave ? -1 : buttonAt(e.event);
			switch (e.event.eventType) {
			case VREvent_MouseMove:
			case VREvent_FocusLeave:
				m.dirty |= at != m.hovered;
				m.hovered = at;
				break;
			case VREvent_MouseButtonDown:
				m.pressed = at;
				m.dirty = true;
				break;
			case VREvent_MouseButtonUp:
				// A click: pressed and let go on the same button
				if (at >= 0 && at == m.pressed)
					answer = m.buttons[at].first;
				m.pressed = -1;
				m.dirty = true;
				break;
			default:
				break;
			}
		}
		if (m.dirty)
			m.Draw();
		// Filled here, on the calling thread (the game's), never on the own frames' thread
		if (!m.uploaded || !m.chain->Live())
			m.uploaded = m.chain->Upload(m.image);
		p.cursorShown = false;
		const int hand = m.mouse.ActiveHand();
		if (std::optional<overlay_maths::Hit> hit = m.mouse.Cursor(hand); hit && m.mouse.Target(hand) == m.Handle())
			p.ShowCursor(overlay_maths::PointOn(m.placement, hit->u, hit->v));
		return answer < 0;
	});

	std::unique_ptr<Pointing::Message> shown;
	{
		std::lock_guard<std::recursive_mutex> lock(layersLock);
		shown = std::move(pointing->message);
		pointing->cursorShown = false;
	}
	shown.reset();
	if (!ran)
		OC_UNSUPPORTED(VRMessageOverlayResponse_CouldntFindSystemOverlay, "ShowMessageOverlay: OpenLamina's own frames stopped");
	return answer >= 0 ? (VRMessageOverlayResponse)answer : VRMessageOverlayResponse_ApplicationQuit;
}
void BaseOverlay::CloseMessageOverlay()
{
	// The message box's frames, on the thread that showed it, end at their next pass
	pointing->closeMessage = true;
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
