#include "stdafx.h"
#include <numbers>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <optional>
#define BASE_IMPL

#include "Misc/Config.h"
#include "Misc/Fade.h"
#include "Misc/FrameStats.h"
#include "Misc/Image.h"
#include "Misc/MirrorWindow.h"

#include "convert.h"

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/transform.hpp>

using glm::mat4;
using glm::quat;
using glm::vec3;
using glm::vec4;

#include "BaseCompositor.h"
#if defined(SUPPORT_GL)
#include "../Compositor/glcompositor.h"
#endif
#include "BaseOverlay.h"

// For the left and right hand constants - TODO move them to their own file
#include "BaseSystem.h"
#include "generated/static_bases.gen.h"

// FIXME find a nice way to clean this up
#ifdef SUPPORT_VK
#include "../../DrvOpenXR/pub/DrvOpenXR.h"
#endif
#include "../../DrvOpenXR/HelperSession.h"

#include "BaseClientCore.h"
#include "Drivers/Backend.h"
#include "Misc/Process.h"
#include "Misc/ScopeGuard.h"
#include "Misc/SharedStore.h"

using namespace vr;
using namespace IVRCompositor_022;

typedef int ovr_enum_t;

#define SESS (*ovr::session)

BaseCompositor::BaseCompositor()
{
}

BaseCompositor::~BaseCompositor()
{
}

void BaseCompositor::SetTrackingSpace(ETrackingUniverseOrigin eOrigin)
{
	XrReferenceSpaceType origin = XR_REFERENCE_SPACE_TYPE_STAGE;
	if (eOrigin == TrackingUniverseSeated) {
		origin = XR_REFERENCE_SPACE_TYPE_LOCAL;
	}

	BaseSystem::currentSpace = origin;
	trackingUniverse = (int)eOrigin;
}

ETrackingUniverseOrigin BaseCompositor::GetTrackingSpace()
{
	return UniverseAnswer((ETrackingUniverseOrigin)trackingUniverse.load(), BaseSystem::currentSpace);
}

ovr_enum_t BaseCompositor::WaitGetPoses(TrackedDevicePose_t* renderPoseArray, uint32_t renderPoseArrayCount,
    TrackedDevicePose_t* gamePoseArray, uint32_t gamePoseArrayCount)
{

	// Assume this method isn't being called between frames, b/c it really shouldn't be.
	leftEyeSubmitted = false;
	rightEyeSubmitted = false;

	// An overlay or background application runs no frames of its own: the poses, and the answer SteamVR gives it
	if (IsHelperSession()) {
		GetLastPoses(renderPoseArray, renderPoseArrayCount, gamePoseArray, gamePoseArrayCount);
		return VRCompositorError_IsNotSceneApplication;
	}

	// A seated zero pose another program stored moves the seated origin, between frames
	BaseSystem::_ApplyStoredSeatedZero();

	BackendManager::Instance().WaitForTrackingData();

	ovr_enum_t result = GetLastPoses(renderPoseArray, renderPoseArrayCount, gamePoseArray, gamePoseArrayCount);
	if (renderPoseArray && renderPoseArrayCount) {
		// The frame's id is its index, which counts display periods; a count of frames where the runtime can't say
		BackendManager* backends = BackendManager::InstancePtr();
		IBackend* backend = backends ? backends->GetBackendInstance() : nullptr;
		const uint32_t frameId = backend ? backend->CurrentFrameId() : 0;
		std::lock_guard<std::mutex> lock(posesLock);
		lastPoseId = frameId && (lastPoseId == 0 || (int32_t)(frameId - lastPoseId) > 0) ? frameId : lastPoseId + 1;
		FramePoses& kept = recentPoses[lastPoseId % std::size(recentPoses)];
		kept.id = lastPoseId;
		kept.poses.assign(renderPoseArray, renderPoseArray + renderPoseArrayCount);
	}
	return result;
}

void BaseCompositor::GetSinglePoseRendering(ETrackingUniverseOrigin origin, TrackedDeviceIndex_t unDeviceIndex, TrackedDevicePose_t* pOutputPose)
{
	BackendManager::Instance().GetSinglePose(origin, unDeviceIndex, pOutputPose, ETrackingStateType::TrackingStateType_Rendering);
}

mat4 BaseCompositor::GetHandTransform()
{
	float deg_to_rad = std::numbers::pi / 180;

	// The angle offset between the Touch and Vive controllers.
	// If this is incorrect, virtual hands will feel off.
	float controller_offset_angle = 39.5;

	// When testing to try and find the correct value above, uncomment
	//  this to lock the controller perfectly flat.
	// ovrPose.ThePose.Orientation = { 0,0,0,1 };

	vec3 rotateAxis = vec3(1, 0, 0);
	quat rotation = glm::rotate(controller_offset_angle * deg_to_rad, rotateAxis); // count++ * 0.01f);

	mat4 transform(rotation);

	// Controller offset
	// Note this is about right, found by playing around in Unity until everything
	//  roughly lines up. If you want to contribute better numbers, please go ahead!
	transform[3] = vec4(0.0f, 0.0353f, -0.0451f, 1.0f);

	return transform;
}

mat4 BaseCompositor::GetTrackerTransform()
{
	// The amount the tracker should be offset
	float tracker_offset = 0.025f;

	mat4 transform(1.0f);

	transform[3] = vec4(0.0f, 0.0f, tracker_offset, 1.0f);

	return transform;
}

ovr_enum_t BaseCompositor::GetLastPoses(TrackedDevicePose_t* renderPoseArray, uint32_t renderPoseArrayCount,
    TrackedDevicePose_t* gamePoseArray, uint32_t gamePoseArrayCount)
{

	ETrackingUniverseOrigin origin = GetTrackingSpace();

	for (uint32_t i = 0; i < std::max(gamePoseArrayCount, renderPoseArrayCount); i++) {
		TrackedDevicePose_t* renderPose = NULL;
		TrackedDevicePose_t* gamePose = NULL;

		if (renderPoseArray) {
			renderPose = i < renderPoseArrayCount ? renderPoseArray + i : NULL;
		}

		if (gamePoseArray) {
			gamePose = i < gamePoseArrayCount ? gamePoseArray + i : NULL;
		}

		if (renderPose) {
			GetSinglePoseRendering(origin, i, renderPose);
		}

		if (gamePose) {
			if (renderPose) {
				*gamePose = *renderPose;
			} else {
				GetSinglePoseRendering(origin, i, gamePose);
			}
		}
	}

	return VRCompositorError_None;
}

ovr_enum_t BaseCompositor::GetLastPoseForTrackedDeviceIndex(TrackedDeviceIndex_t unDeviceIndex, TrackedDevicePose_t* pOutputPose,
    TrackedDevicePose_t* pOutputGamePose)
{

	if (unDeviceIndex >= k_unMaxTrackedDeviceCount) {
		return VRCompositorError_IndexOutOfRange;
	}

	TrackedDevicePose_t pose{};
	GetSinglePoseRendering(GetTrackingSpace(), unDeviceIndex, &pose);

	if (pOutputPose) {
		*pOutputPose = pose;
	}

	if (pOutputGamePose) {
		*pOutputGamePose = pose;
	}

	return VRCompositorError_None;
}

std::unique_ptr<Compositor> BaseCompositor::CreateCompositorAPI(const vr::Texture_t* texture)
{
	std::unique_ptr<Compositor> comp;

	switch (texture->eType) {
#if defined(SUPPORT_GL)
	case TextureType_OpenGL: {
		// Double-cast to avoid a CLion warning
		comp = std::make_unique<GLCompositor>((GLuint)(intptr_t)texture->handle);
		break;
	}
#elif defined(SUPPORT_GLES)
	case TextureType_OpenGL: {
		// Double-cast to avoid a CLion warning
		comp = std::make_unique<GLESCompositor>();
		break;
	}
#endif
#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	case TextureType_DirectX: {
#if defined(SUPPORT_DX10)
		// A Direct3D 10 texture comes as TextureType_DirectX too: told apart by its interface (dx10Mode is ignored)
		if (ID3D10Texture2D* tex10 = DX10Compositor::AsD3D10Texture(texture)) {
			comp = DX10Compositor::Create(tex10);
			if (!comp)
				OOVR_LOG_ONCE("A Direct3D 10 texture without a Direct3D 11 device to copy it through: no compositor");
			break;
		}
#endif
		comp = std::make_unique<DX11Compositor>((ID3D11Texture2D*)texture->handle);
		break;
	}
#endif
#ifdef SUPPORT_VK
	case TextureType_Vulkan: {
		comp = std::make_unique<VkCompositor>(texture);
		break;
	}
#endif
#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)
	case TextureType_DirectX12: {
		comp = std::make_unique<DX12Compositor>((D3D12TextureData_t*)texture->handle);
		break;
	}
#endif
	default:
		// No compositor: the eye is left out, and Submit answers an error
		OOVR_LOG_ONCEF("[BaseCompositor::Submit] Unsupported texture type: %d", (int)texture->eType);
		break;
	}

	return comp;
}

// Whether another OpenLamina program than this one is running and has put its overlays in the store
static bool OtherProgramsAround()
{
	return SharedStore::OtherLiveProgram(SharedStore::Get().Read(StoreDoc::Overlays), oc_current_pid());
}

// The scene app's frame statistics in the Scene document, for other programs' GetCumulativeStats: at most once a
// second, and only while another OpenLamina program is around to read them. Written after the frame was handed over,
// by the store's writer thread, never waited for.
static void PublishSceneStats()
{
	// Called for each eye, maybe from two threads: the compare-exchange lets one of them through each second
	static std::atomic<int64_t> lastMs{ 0 };
	const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	int64_t last = lastMs.load();
	if (BaseClientCore::appType != vr::VRApplication_Scene || (last != 0 && now - last < 1000) || !lastMs.compare_exchange_strong(last, now))
		return;
	if (!OtherProgramsAround())
		return;
	OOVR_Compositor_CumulativeStats stats{};
	FrameStats::Get().Cumulative(oc_current_pid(), &stats, sizeof(stats));
	const uint32_t self = oc_current_pid();
	// Written by the store's writer thread: the frame doesn't wait for the file
	SharedStore::Get().UpdateLater(StoreDoc::Scene, "frame_stats", [self, stats](Json::Value& scene) {
		// Only while it's this program's
		if (!scene.isObject() || !scene["pid"].isUInt() || scene["pid"].asUInt() != self)
			return;
		scene["frame_stats"]["presents"] = stats.m_nNumFramePresents;
		scene["frame_stats"]["dropped"] = stats.m_nNumDroppedFrames;
		scene["frame_stats"]["reprojected"] = stats.m_nNumReprojectedFrames;
	});
}

ovr_enum_t BaseCompositor::Submit(EVREye eye, const Texture_t* texture, const VRTextureBounds_t* bounds, EVRSubmitFlags submitFlags)
{
	if (BaseClientCore::appType == vr::VRApplication_Background || IsHelperSession()) {
		// As SteamVR answers a Background or Overlay app that submits
		OOVR_LOG_ONCE("A VRApplication_Background or Overlay app is submitting frames: refused");
		return VRCompositorError_IsNotSceneApplication;
	}
	if (!texture || (eye != Eye_Left && eye != Eye_Right))
		return VRCompositorError_InvalidTexture;
	if (!BoundsAreNumbers(bounds)) {
		OOVR_LOG_ONCE("Texture bounds that aren't numbers: Submit refused");
		return VRCompositorError_InvalidBounds;
	}

	std::lock_guard<std::mutex> submitGuard(submitLock);
	bool isFirstEye = !leftEyeSubmitted && !rightEyeSubmitted;
	ovr_enum_t result = VRCompositorError_None;

	bool eyeState = false;
	if (eye == Eye_Left)
		eyeState = leftEyeSubmitted;
	else
		eyeState = rightEyeSubmitted;

	if (eyeState) {
		// The first texture for this eye stays
		OOVR_LOG_ONCE("An eye was submitted twice in one frame: the second is refused");
		return VRCompositorError_AlreadySubmitted;
	}

	if (eye == Eye_Left)
		leftEyeSubmitted = true;
	else
		rightEyeSubmitted = true;

	// Handle null textures
	// Rather surprisingly, it's perfectly valid to pass null textures to SteamVR. So far, I've
	// only seen this used in Sparc (see #19). If a game tries to do this, ensure that either neither or
	// both eyes do so, and don't actually send a frame to LibOVR.
	bool textureNull = texture->handle == nullptr;
	if (isFirstEye) {
		isNullRender = textureNull;
	} else if (textureNull != isNullRender) {
		// One eye null and the other not: this frame isn't submitted, and the next starts afresh
		OOVR_LOG_ONCE("One eye's texture is null and the other's isn't: that frame is dropped");
		leftEyeSubmitted = false;
		rightEyeSubmitted = false;
		return VRCompositorError_InvalidTexture;
	}

	{
		// Turns out games (Beat Saber in particular) really do not like the session being restarted
		// while they're in the middle of submitting a frame.
		auto lock = xr_session.lock_shared();
		if (!textureNull)
			result = (ovr_enum_t)BackendManager::Instance().StoreEyeTexture(eye, texture, bounds, submitFlags, isFirstEye);

		if (leftEyeSubmitted && rightEyeSubmitted) {
			if (!isNullRender && SubmitEndsFrame(explicitTimingMode))
				BackendManager::Instance().SubmitFrames(isInSkybox);

			leftEyeSubmitted = false;
			rightEyeSubmitted = false;
		}
	}
	PublishSceneStats();

	return result;
}

ovr_enum_t BaseCompositor::SubmitWithArrayIndex(vr::EVREye eye, const vr::Texture_t* texture, uint32_t idx, const vr::VRTextureBounds_t* bounds, vr::EVRSubmitFlags submitFlags)
{
	// The texture is one array texture, and idx its slice (not an index into an array of Texture_t): the compositors
	// show that slice (D3D11, D3D12 and OpenGL arrays; Vulkan's say theirs in VRVulkanTextureArrayData_t)
	if (!texture)
		return VRCompositorError_InvalidTexture;
	SubmitArraySlice slice(idx);
	return Submit(eye, texture, bounds, submitFlags);
}

void BaseCompositor::ClearLastSubmittedFrame()
{
	// Nothing of the game's last frame is shown until it submits again
	if (IsHelperSession())
		return;
	BackendManager::Instance().ClearSubmittedFrame();
}

void BaseCompositor::PostPresentHandoff()
{
	// It appears (from the documentation) that SteamVR will, even after all frames are submitted, not begin
	//  compositing the submitted textures until WaitGetPoses is called. Thus is you want to do some rendering
	//  or game logic or whatever, it will delay the compositor. Calling this tells SteamVR that no further changes
	//  are to be made to the frame, and it can begin the compositor - in the aforementioned cases, this would be
	//  called directly after the last Submit call.
	//
	// Some apps provide a GUI through layers that are submitted after any eye textures are submitted. The app then
	// calls PostPresentHandOff to signal that all data is submitted and compositor can start work. Mimick this by
	// ending the frame here; in the timing mode where the game says it does that
	// (Explicit_ApplicationPerformsPostPresentHandoff), the second Submit doesn't. In any other mode the frame was
	// ended there already, and this ends none.

	// An overlay or background application has no frame of its own to end
	if (IsHelperSession())
		return;
	// As in Submit: the session isn't restarted while the frame is ended
	auto lock = xr_session.lock_shared();
	BackendManager::Instance().SubmitFrames(isInSkybox);
}

bool BaseCompositor::GetFrameTiming(OOVR_Compositor_FrameTiming* pTiming, uint32_t unFramesAgo)
{
	// "Sets oldest timing info if nFramesAgo is larger than the stored history"
	if (unFramesAgo > 0 && pTiming && pTiming->m_nSize >= sizeof(uint32_t)) {
		uint32_t count = (uint32_t)std::min<uint64_t>((uint64_t)unFramesAgo + 1, FrameStats::kHistory);
		std::vector<OOVR_Compositor_FrameTiming> recent(count);
		recent[0].m_nSize = sizeof(OOVR_Compositor_FrameTiming);
		uint32_t got = FrameStats::Get().Timings(recent.data(), count);
		if (got > 0) {
			uint32_t size = std::min<uint32_t>(pTiming->m_nSize, sizeof(OOVR_Compositor_FrameTiming));
			memcpy(pTiming, &recent[0], size);
			pTiming->m_nSize = size;
			return true;
		}
	}

	return BackendManager::Instance().GetFrameTiming(pTiming, unFramesAgo);

	// TODO fill in the m_nNumVSyncsReadyForUse and uint32_t m_nNumVSyncsToFirstView fields, but only
	// when called from the correct version of the interface.
}

uint32_t BaseCompositor::GetFrameTimings(OOVR_Compositor_FrameTiming* pTiming, uint32_t nFrames)
{
	// This is a request to fill out an array of timing data. However only an arbitrary number
	// of records are available with number being filled returned. In the case of only one record
	// being available we can just send the most recent timing data and return 1.
	if (nFrames == 0)
		return 0;
	if (uint32_t got = FrameStats::Get().Timings(pTiming, nFrames))
		return got;
	bool populated = BackendManager::Instance().GetFrameTiming(pTiming, 1);
	return populated ? 1 : 0;
}

bool BaseCompositor::GetFrameTiming(vr::Compositor_FrameTiming* pTiming, uint32_t unFramesAgo)
{
	return GetFrameTiming((OOVR_Compositor_FrameTiming*)pTiming, unFramesAgo);
}

uint32_t BaseCompositor::GetFrameTimings(vr::Compositor_FrameTiming* pTiming, uint32_t nFrames)
{
	return GetFrameTimings((OOVR_Compositor_FrameTiming*)pTiming, nFrames);
}

float BaseCompositor::GetFrameTimeRemaining()
{
	return FrameStats::Get().SecondsRemaining(FrameStats::Clock::now());
}

void BaseCompositor::GetCumulativeStats(OOVR_Compositor_CumulativeStats* pStats, uint32_t nStatsSizeInBytes)
{
	if (!pStats || nStatsSizeInBytes == 0)
		return;
	if (IsHelperSession()) {
		// An overlay or background application's: the scene app's frames, which it put in the store
		Json::Value scene = SharedStore::Get().Read(StoreDoc::Scene);
		OOVR_Compositor_CumulativeStats stats{};
		const uint32_t pid = scene["pid"].isUInt() ? scene["pid"].asUInt() : 0;
		if (pid && SharedStore::IsSameProcessAlive(pid, scene["start"].isUInt64() ? scene["start"].asUInt64() : 0)) {
			stats.m_nPid = pid;
			stats.m_nNumFramePresents = scene["frame_stats"]["presents"].asUInt();
			stats.m_nNumDroppedFrames = scene["frame_stats"]["dropped"].asUInt();
			stats.m_nNumReprojectedFrames = scene["frame_stats"]["reprojected"].asUInt();
		}
		memset(pStats, 0, nStatsSizeInBytes);
		memcpy(pStats, &stats, std::min<size_t>(nStatsSizeInBytes, sizeof(stats)));
		return;
	}
	FrameStats::Get().Cumulative(oc_current_pid(), pStats, nStatsSizeInBytes);
}

void BaseCompositor::FadeToColor(float fSeconds, float fRed, float fGreen, float fBlue, float fAlpha, bool bBackground)
{
	// Drawn over the scene by the backend. The background's fade is kept and answered, but not drawn.
	CompositorFades::Get().FadeToColour(CompositorClockSeconds(), fSeconds, { fRed, fGreen, fBlue, fAlpha }, bBackground);
}

HmdColor_t BaseCompositor::GetCurrentFadeColor(bool bBackground)
{
	return CompositorFades::Get().ColourAt(CompositorClockSeconds(), bBackground);
}

void BaseCompositor::FadeGrid(float fSeconds, bool bFadeIn)
{
	// The app fading from the scene into the skybox, eg before it loads a new level. Once the grid is fully in, the
	// skybox override is shown in place of the scene; no grid is drawn.
	CompositorFades::Get().FadeGrid(CompositorClockSeconds(), fSeconds, bFadeIn);
	isInSkybox = bFadeIn;
}

float BaseCompositor::GetCurrentGridAlpha()
{
	return CompositorFades::Get().GridAlphaAt(CompositorClockSeconds());
}

ovr_enum_t BaseCompositor::SetSkyboxOverride(const Texture_t* pTextures, uint32_t unTextureCount)
{
	// The scene app's: SteamVR answers an overlay or background application DoNotHaveFocus
	if (IsHelperSession())
		return VRCompositorError_DoNotHaveFocus;
	// One lat-long texture, a stereo pair or six cube faces
	if (!SkyboxTexturesValid(pTextures, unTextureCount))
		return VRCompositorError_InvalidTexture;
	return BackendManager::Instance().SetSkyboxOverride(pTextures, unTextureCount);
}

uint32_t BaseCompositor::GetLastError(char* pchBuffer, uint32_t unBufferSize)
{
	// Errors are answered by each call's result; there's no last error kept
	if (pchBuffer && unBufferSize)
		pchBuffer[0] = '\0';
	return 0;
}

void BaseCompositor::SetVSync(bool bVSync)
{
	oldVSync = bVSync;
}

bool BaseCompositor::GetVSync()
{
	return oldVSync;
}

void BaseCompositor::SetGamma(float fGamma)
{
	oldGamma = fGamma;
}

float BaseCompositor::GetGamma()
{
	return oldGamma;
}

void BaseCompositor::SetGraphicsDevice(OOVR_Compositor_DeviceType eType, void* pDevice)
{
	// The device itself comes with each texture (D3D11's GetDevice, OpenGL's current context), as for today's Submit
	oldDeviceType = eType;
	overlayReader.reset(); // made again for the new device's textures
}

bool BaseCompositor::OldDeviceTextureType(ETextureType* pType) const
{
	switch (oldDeviceType) {
	case OOVR_Compositor_DeviceType_D3D11:
		*pType = TextureType_DirectX;
		return true;
	case OOVR_Compositor_DeviceType_OpenGL:
		*pType = TextureType_OpenGL;
		return true;
#if defined(SUPPORT_DX10)
	case OOVR_Compositor_DeviceType_D3D10:
		// A D3D10 texture is TextureType_DirectX, told apart from D3D11's by its interface
		*pType = TextureType_DirectX;
		return true;
#endif
	default:
		// None (SetGraphicsDevice wasn't called), D3D9 or D3D9Ex: nothing OpenLamina can show
		return false;
	}
}

BaseCompositor::ovr_enum_t BaseCompositor::SubmitForDevice(EVREye eEye, void* pTexture, const VRTextureBounds_t* pBounds)
{
	Texture_t texture = { pTexture, TextureType_DirectX, ColorSpace_Auto };
	if (!OldDeviceTextureType(&texture.eType))
		OC_UNSUPPORTED(VRCompositorError_InvalidTexture, "Submit (IVRCompositor_005/006): a texture of device type %d "
		                                                 "(only D3D10, D3D11 and OpenGL can be shown)", (int)oldDeviceType);
	return Submit(eEye, &texture, pBounds, Submit_Default);
}

BaseCompositor::ovr_enum_t BaseCompositor::SubmitWithConvention(EVREye eEye, EGraphicsAPIConvention eTextureType, void* pTexture,
    const VRTextureBounds_t* pBounds, EVRSubmitFlags nSubmitFlags)
{
	// API_DirectX and API_OpenGL are TextureType_DirectX and TextureType_OpenGL
	Texture_t texture = { pTexture, (ETextureType)eTextureType, ColorSpace_Auto };
	return Submit(eEye, &texture, pBounds, nSubmitFlags);
}

BaseCompositor::ovr_enum_t BaseCompositor::SetSkyboxOverrideWithConvention(EGraphicsAPIConvention eTextureType, void* pFront,
    void* pBack, void* pLeft, void* pRight, void* pTop, void* pBottom)
{
	Texture_t textures[6];
	void* faces[6] = { pFront, pBack, pLeft, pRight, pTop, pBottom };
	for (int i = 0; i < 6; i++)
		textures[i] = { faces[i], (ETextureType)eTextureType, ColorSpace_Auto };
	return SetSkyboxOverride(textures, 6);
}

void BaseCompositor::GetOverlayDefaults(Compositor_OverlaySettings* pSettings)
{
	if (!pSettings)
		return;
	// A flat, opaque quad a metre away, its whole texture
	*pSettings = {};
	pSettings->size = sizeof(Compositor_OverlaySettings);
	pSettings->antialias = true;
	pSettings->scale = 1.0f;
	pSettings->distance = 1.0f;
	pSettings->alpha = 1.0f;
	pSettings->uScale = 1.0f;
	pSettings->vScale = 1.0f;
	for (int i = 0; i < 4; i++)
		pSettings->transform.m[i][i] = 1.0f;
}

bool BaseCompositor::SceneOverlayFrom(const Image& source, const Compositor_OverlaySettings& settings, SceneOverlayImage& out)
{
	if (source.Empty() || source.rgba.size() != (size_t)source.width * source.height * 4)
		return false;
	// The part of the image picked, in pixels (a scale of zero or less, or not a number, is the whole image)
	auto finite = [](float v, float otherwise) { return std::isfinite(v) ? v : otherwise; };
	const float uOffset = finite(settings.uOffset, 0.0f), vOffset = finite(settings.vOffset, 0.0f);
	const float uScale = finite(settings.uScale, 1.0f) > 0.0f ? finite(settings.uScale, 1.0f) : 1.0f;
	const float vScale = finite(settings.vScale, 1.0f) > 0.0f ? finite(settings.vScale, 1.0f) : 1.0f;
	auto pixel = [](float v, uint32_t size) { return (uint32_t)std::lround(std::clamp(v, 0.0f, 1.0f) * (float)size); };
	const uint32_t x0 = pixel(uOffset, source.width), x1 = pixel(uOffset + uScale, source.width);
	const uint32_t y0 = pixel(vOffset, source.height), y1 = pixel(vOffset + vScale, source.height);
	if (x1 <= x0 || y1 <= y0)
		return false;
	const float alpha = std::isfinite(settings.alpha) ? std::clamp(settings.alpha, 0.0f, 1.0f) : 1.0f;
	out.image.width = x1 - x0;
	out.image.height = y1 - y0;
	out.image.rgba.resize((size_t)out.image.width * out.image.height * 4);
	for (uint32_t y = y0; y < y1; y++) {
		const uint8_t* from = &source.rgba[((size_t)y * source.width + x0) * 4];
		uint8_t* to = &out.image.rgba[(size_t)(y - y0) * out.image.width * 4];
		std::memcpy(to, from, (size_t)out.image.width * 4);
		if (alpha < 1.0f)
			for (uint32_t i = 3; i < out.image.width * 4; i += 4)
				to[i] = (uint8_t)std::lround(to[i] * alpha);
	}

	// The settings' transform (none: all zeroes, or not finite), then the distance ahead
	glm::mat4 transform = glm::transpose(glm::make_mat4(&settings.transform.m[0][0]));
	bool usable = false;
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++) {
			if (!std::isfinite(transform[c][r]))
				transform = glm::mat4(1.0f);
			usable = usable || transform[c][r] != 0.0f;
		}
	if (!usable)
		transform = glm::mat4(1.0f);
	const float distance = std::isfinite(settings.distance) && settings.distance > 0.0f ? settings.distance : 1.0f;
	out.inHead = G2S_m34(transform * glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -distance)));
	out.width = std::isfinite(settings.scale) && settings.scale > 0.0f ? settings.scale : 1.0f;
	return true;
}

void BaseCompositor::ShowSceneOverlay(const Image& image, const Compositor_OverlaySettings* pSettings)
{
	BackendManager* backends = BackendManager::InstancePtr();
	IBackend* backend = backends ? backends->GetBackendInstance() : nullptr;
	if (!backend)
		return;
	Compositor_OverlaySettings settings;
	GetOverlayDefaults(&settings);
	// An older struct is as long as its size says
	if (pSettings && pSettings->size > 0)
		memcpy(&settings, pSettings, std::min<size_t>(pSettings->size, sizeof(settings)));
	auto overlay = std::make_shared<SceneOverlayImage>();
	if (!SceneOverlayFrom(image, settings, *overlay)) {
		backend->SetSceneOverlay(nullptr);
		return;
	}
	backend->SetSceneOverlay(std::move(overlay));
}

void BaseCompositor::SetOverlay(void* pTexture, Compositor_OverlaySettings* pSettings)
{
	// The texture, on the device SetGraphicsDevice named, read back
	Texture_t texture = { pTexture, TextureType_DirectX, ColorSpace_Auto };
	if (!pTexture || !OldDeviceTextureType(&texture.eType)) {
		OOVR_LOG_ONCE("SetOverlay: a texture of a device type OpenLamina can't read (only D3D10, D3D11 and OpenGL)");
		return;
	}
	if (!overlayReader)
		overlayReader = CreateCompositorAPI(&texture);
	Image image;
	if (!overlayReader || !overlayReader->ReadbackToCpu(&texture, nullptr, image)) {
		OOVR_LOG_ONCE("SetOverlay: the texture couldn't be read back");
		return;
	}
	ShowSceneOverlay(image, pSettings);
}

void BaseCompositor::SetOverlayRaw(void* buffer, uint32_t width, uint32_t height, uint32_t depth, Compositor_OverlaySettings* pSettings)
{
	// Grey, RGB or RGBA bytes, rows top to bottom
	ShowSceneOverlay(ConvertToRgba8(buffer, width, height, width * depth, depth), pSettings);
}

void BaseCompositor::SetOverlayFromFile(const char* pchFilePath, Compositor_OverlaySettings* pSettings)
{
	if (!pchFilePath || !*pchFilePath)
		return;
	std::string why;
	std::optional<Image> image = LoadImageFile(std::filesystem::path(std::u8string((const char8_t*)pchFilePath)), &why);
	if (!image) {
		OOVR_LOGF("SetOverlayFromFile: %s isn't shown: %s", pchFilePath, why.c_str());
		return;
	}
	ShowSceneOverlay(*image, pSettings);
}

void BaseCompositor::ClearOverlay()
{
	if (BackendManager* backends = BackendManager::InstancePtr())
		if (IBackend* backend = backends->GetBackendInstance())
			backend->SetSceneOverlay(nullptr);
}

bool BaseCompositor::ComputeOverlayIntersection(const Compositor_OverlaySettings* pSettings, float fAspectRatio,
    ETrackingUniverseOrigin eOrigin, HmdVector3_t vSource, HmdVector3_t vDirection,
    HmdVector2_t* pvecIntersectionUV, HmdVector3_t* pvecIntersectionTrackingSpace)
{
	// No overlay is drawn: no intersection
	return false;
}

void BaseCompositor::ClearSkyboxOverride()
{
	BackendManager::Instance().ClearSkyboxOverride();
}

void BaseCompositor::CompositorBringToFront()
{
	// No actions required, Oculus runs via direct mode
}

void BaseCompositor::CompositorGoToBack()
{
	// The OpenXR runtime's compositor owns the headset; there's nothing to send back
}

void BaseCompositor::CompositorQuit()
{
	// The OpenXR runtime's compositor isn't ours to quit
}

bool BaseCompositor::IsFullscreen()
{
	// Direct mode, as SteamVR's compositor
	return true;
}

uint32_t BaseCompositor::GetCurrentSceneFocusProcess()
{
	// An overlay or background application's: the scene application's, as it put in the store
	if (IsHelperSession()) {
		Json::Value scene = SharedStore::Get().Read(StoreDoc::Scene);
		const uint32_t pid = scene["pid"].isUInt() ? scene["pid"].asUInt() : 0;
		if (pid && SharedStore::IsSameProcessAlive(pid, scene["start"].isUInt64() ? scene["start"].asUInt64() : 0))
			return pid;
		return 0;
	}
	return oc_current_pid();
}

uint32_t BaseCompositor::GetLastFrameRenderer()
{
	return oc_current_pid();
}

bool BaseCompositor::CanRenderScene()
{
	return true; // TODO implement
}

void BaseCompositor::ShowMirrorWindow()
{
	// Only where the user asked for it: its image is read back from the game's frames, which costs frame time
	if (!oovr_global_configuration.MirrorWindow()) {
		OOVR_LOG_ONCE("ShowMirrorWindow: the game asks for a mirror window, which mirrorWindow=true in openlamina.ini shows");
		return;
	}
	MirrorWindow::Get().Show();
}

void BaseCompositor::HideMirrorWindow()
{
	MirrorWindow::Get().Hide();
}

bool BaseCompositor::IsMirrorWindowVisible()
{
	return MirrorWindow::Get().Visible();
}

namespace {
// The eyes whose next image CompositorDumpImages wants: bit 0 the left, bit 1 the right
std::atomic<int> dumpEyesWanted{ 0 };
} // namespace

void BaseCompositor::CompositorDumpImages()
{
	if (IsHelperSession())
		return; // an overlay or background application submits no eyes
	dumpEyesWanted = 3;
}

bool BaseCompositor::_WantsDumpImage(vr::EVREye eye)
{
	return (dumpEyesWanted.load() & (1 << (int)eye)) != 0;
}

void BaseCompositor::_OfferDumpImage(vr::EVREye eye, const Image& image)
{
	// Once per call: the first image of each eye after it
	if (!(dumpEyesWanted.fetch_and(~(1 << (int)eye)) & (1 << (int)eye)))
		return;
	const std::string folder = oovr_log_folder();
	const std::filesystem::path path = folder.empty() ? std::filesystem::path(DumpImageName(oc_current_pid(), eye))
	                                                  : std::filesystem::path(folder) / DumpImageName(oc_current_pid(), eye);
	std::string why;
	if (WritePngFile(image, path, &why))
		OOVR_LOGF("CompositorDumpImages: wrote %s", path.string().c_str());
	else
		OOVR_LOGF("CompositorDumpImages: couldn't write %s: %s", path.string().c_str(), why.c_str());
}

bool BaseCompositor::ShouldAppRenderWithLowResources()
{
	// TODO put in config file
	return false;
}

void BaseCompositor::ForceInterleavedReprojectionOn(bool bOverride)
{
	// Force timewarp on? Yeah right.
}

void BaseCompositor::ForceReconnectProcess()
{
	// We should always be connected
}

void BaseCompositor::SuspendRendering(bool bSuspend)
{
	// The game's frames aren't shown while suspended; it keeps submitting them as usual
	if (IsHelperSession())
		return;
	BackendManager::Instance().SetRenderingSuspended(bSuspend);
}

ovr_enum_t BaseCompositor::GetMirrorTextureD3D11(EVREye eEye, void* pD3D11DeviceOrResource, void** ppD3D11ShaderResourceView)
{
#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	return BackendManager::Instance().GetMirrorTextureD3D11(eEye, pD3D11DeviceOrResource, ppD3D11ShaderResourceView);
#else
	if (ppD3D11ShaderResourceView)
		*ppD3D11ShaderResourceView = nullptr;
	OC_UNSUPPORTED(VRCompositorError_RequestFailed, "GetMirrorTextureD3D11: this build has no D3D11 support");
#endif
}

void BaseCompositor::ReleaseMirrorTextureD3D11(void* pD3D11ShaderResourceView)
{
#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	return BackendManager::Instance().ReleaseMirrorTextureD3D11(pD3D11ShaderResourceView);
#else
	// GetMirrorTextureD3D11 handed nothing out in this build
#endif
}

ovr_enum_t BaseCompositor::GetMirrorTextureGL(EVREye eEye, glUInt_t* pglTextureId, glSharedTextureHandle_t* pglSharedTextureHandle)
{
	if (pglTextureId)
		*pglTextureId = 0;
	if (pglSharedTextureHandle)
		*pglSharedTextureHandle = nullptr;
#if defined(SUPPORT_GL)
	if (eEye != Eye_Left && eEye != Eye_Right)
		return VRCompositorError_RequestFailed;
	// In the game's context (the one current on this thread), at the size the game renders at
	uint32_t width = 0, height = 0;
	BackendManager::Instance().GetPrimaryHMD()->GetRecommendedRenderTargetSize(&width, &height);
	const GLuint texture = GLMirrorGet((int)eEye, width, height);
	if (!texture)
		return VRCompositorError_RequestFailed;
	if (pglTextureId)
		*pglTextureId = texture;
	// The handle is the texture's own name: the mirror lives in the game's context, so there's nothing to share
	if (pglSharedTextureHandle)
		*pglSharedTextureHandle = (glSharedTextureHandle_t)(uintptr_t)texture;
	return VRCompositorError_None;
#else
	OC_UNSUPPORTED(VRCompositorError_RequestFailed, "GetMirrorTextureGL: this build has no OpenGL support");
#endif
}

bool BaseCompositor::ReleaseSharedGLTexture(glUInt_t glTextureId, glSharedTextureHandle_t glSharedTextureHandle)
{
#if defined(SUPPORT_GL)
	return GLMirrorRelease(glTextureId);
#else
	return false; // GetMirrorTextureGL handed nothing out
#endif
}

void BaseCompositor::LockGLSharedTextureForAccess(glSharedTextureHandle_t glSharedTextureHandle)
{
	// The texture is in the game's own context, written by its own Submit calls: nothing to lock
}

void BaseCompositor::UnlockGLSharedTextureForAccess(glSharedTextureHandle_t glSharedTextureHandle)
{
	// Nothing was locked
}

uint32_t BaseCompositor::GetVulkanInstanceExtensionsRequired(char* pchValue, uint32_t unBufferSize)
{
	if (pchValue && unBufferSize)
		pchValue[0] = 0;
#if defined(SUPPORT_VK)
#if defined(_WIN32) && defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	// A runtime with D3D11 but no Vulkan gets the game's frames through D3D11, which needs the game's Vulkan to share memory
	if (!xr_ext->xrGetVulkanInstanceExtensionsKHR_Available() && xr_ext->xrGetD3D11GraphicsRequirementsKHR_Available())
		return ExtensionListSize(InteropVulkanExtensions(false), pchValue, unBufferSize);
#endif
	// Without a buffer, only the size is asked for (a runtime writes into whatever buffer it's given)
	// Whaddya know, the OpenXR, Oculus and Valve methods work almost identically...
	uint32_t size = 0;
	XrResult res = xr_ext->xrGetVulkanInstanceExtensionsKHR(xr_instance, xr_system, pchValue ? unBufferSize : 0, &size, pchValue);
	if (res != XR_ERROR_SIZE_INSUFFICIENT)
		OC_XR_SOFT(res, 0);
	return VulkanExtensionsSize(res, size);
#else
	OC_UNSUPPORTED(0, "GetVulkanInstanceExtensionsRequired: this build has no Vulkan support");
#endif
}

uint32_t BaseCompositor::GetVulkanDeviceExtensionsRequired(VkPhysicalDevice_T* pPhysicalDevice, char* pchValue, uint32_t unBufferSize)
{
	if (pchValue && unBufferSize)
		pchValue[0] = 0;
#if defined(SUPPORT_VK)
#if defined(_WIN32) && defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	if (!xr_ext->xrGetVulkanDeviceExtensionsKHR_Available() && xr_ext->xrGetD3D11GraphicsRequirementsKHR_Available())
		return ExtensionListSize(InteropVulkanExtensions(true), pchValue, unBufferSize);
#endif
	uint32_t size = 0;
	XrResult res = xr_ext->xrGetVulkanDeviceExtensionsKHR(xr_instance, xr_system, pchValue ? unBufferSize : 0, &size, pchValue);
	if (res != XR_ERROR_SIZE_INSUFFICIENT)
		OC_XR_SOFT(res, 0);
	return VulkanExtensionsSize(res, size);
#else
	OC_UNSUPPORTED(0, "GetVulkanDeviceExtensionsRequired: this build has no Vulkan support");
#endif
}

void BaseCompositor::SetExplicitTimingMode(ovr_enum_t eTimingMode)
{
	// Explicit timing means the application calls SubmitExplicitTimingData each
	// frame, and in return we're not allowed to use their Vulkan queue
	// during WaitGetPoses. We never use their queue there, so only the mode is kept.
	explicitTimingMode = (int32_t)eTimingMode;
}

ovr_enum_t BaseCompositor::SubmitExplicitTimingData()
{
	// In SteamVR this records a more accurate timestamp for tracking via the GPU's
	// clock, which OpenXR has no equivalent for, so there is nothing to record.
	if (explicitTimingMode == 0) // the implicit timing mode
		return VRCompositorError_RequestFailed;
	return VRCompositorError_None;
}

bool BaseCompositor::IsMotionSmoothingSupported()
{
	return false;
}

bool BaseCompositor::IsMotionSmoothingEnabled()
{
	return false;
}

bool BaseCompositor::IsCurrentSceneFocusAppLoading()
{
	return isInSkybox;
}

ovr_enum_t BaseCompositor::SetStageOverride_Async(const char* pchRenderModelPath, const HmdMatrix34_t* pTransform,
    const OOVR_Compositor_StageRenderSettings* pRenderSettings, uint32_t nSizeOfRenderSettings)
{
	OOVR_SOFT_ABORT("Stage override not implemented");
	return VRCompositorError_None;
}

void BaseCompositor::ClearStageOverride()
{
	OOVR_SOFT_ABORT("Stage override not implemented");
}

bool BaseCompositor::GetCompositorBenchmarkResults(Compositor_BenchmarkResults* pBenchmarkResults, uint32_t nSizeOfBenchmarkResults)
{
	if (!pBenchmarkResults)
		return true;

	// No benchmark is run: the measured rate is taken to be the recommended one
	uint32_t width = 0, height = 0;
	BackendManager::Instance().GetPrimaryHMD()->GetRecommendedRenderTargetSize(&width, &height);
	vr::ETrackedPropertyError err = vr::TrackedProp_Success;
	float hz = BackendManager::Instance().GetPrimaryHMD()->GetFloatTrackedDeviceProperty(vr::Prop_DisplayFrequency_Float, &err);
	float rate = err == vr::TrackedProp_Success ? MegaPixelsPerSecond(width, height, hz) : 0.0f;

	// An older struct is as long as its size says
	vr::Compositor_BenchmarkResults results{ rate, rate };
	memset(pBenchmarkResults, 0, nSizeOfBenchmarkResults);
	memcpy(pBenchmarkResults, &results, std::min<size_t>(nSizeOfBenchmarkResults, sizeof(results)));
	return true;
}

ovr_enum_t BaseCompositor::GetLastPosePredictionIDs(uint32_t* pRenderPosePredictionID, uint32_t* pGamePosePredictionID)
{
	// The render and game poses come from the same WaitGetPoses
	std::lock_guard<std::mutex> lock(posesLock);
	if (pRenderPosePredictionID)
		*pRenderPosePredictionID = lastPoseId;
	if (pGamePosePredictionID)
		*pGamePosePredictionID = lastPoseId;
	return VRCompositorError_None;
}

ovr_enum_t BaseCompositor::GetPosesForFrame(uint32_t unPosePredictionID, TrackedDevicePose_t* pPoseArray, uint32_t unPoseArrayCount)
{
	if (pPoseArray)
		memset(pPoseArray, 0, sizeof(TrackedDevicePose_t) * unPoseArrayCount);
	int32_t ahead = 0;
	{
		std::lock_guard<std::mutex> lock(posesLock);
		for (const FramePoses& kept : recentPoses) {
			if (kept.id == 0 || kept.id != unPosePredictionID)
				continue;
			if (pPoseArray)
				memcpy(pPoseArray, kept.poses.data(), sizeof(TrackedDevicePose_t) * std::min<size_t>(unPoseArrayCount, kept.poses.size()));
			return VRCompositorError_None;
		}
		ahead = lastPoseId ? (int32_t)(unPosePredictionID - lastPoseId) : 0;
	}
	// A frame to come, a few display periods ahead: the poses predicted for its display
	BackendManager* backends = BackendManager::InstancePtr();
	IBackend* backend = backends ? backends->GetBackendInstance() : nullptr;
	if (ahead > 0 && ahead <= (int32_t)std::size(recentPoses) && backend && backend->CurrentFrameId()) {
		const float seconds = backend->SecondsToFrame(unPosePredictionID);
		if (std::isfinite(seconds)) {
			if (pPoseArray && unPoseArrayCount)
				backends->GetDeviceToAbsoluteTrackingPose(GetTrackingSpace(), seconds, pPoseArray, unPoseArrayCount);
			return VRCompositorError_None;
		}
	}
	// Older than the last few frames, or never given
	return VRCompositorError_IndexOutOfRange;
}

ovr_enum_t BaseCompositor::GetSubmitTexture(Texture_t* pOutTexture, bool* pNeedsFlush, OOVR_EVRCompositorTextureUsage eUsage,
    const Texture_t* pTexture, const VRTextureBounds_t* pBounds, EVRSubmitFlags nSubmitFlags)
{
	// There's no compositor-owned shared texture to give out.
	return VRCompositorError_RequestFailed;
}
