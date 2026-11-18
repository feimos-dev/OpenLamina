//
// Created by ZNix on 25/10/2020.
//

#include "XrBackend.h"
#include "CpuSwapchain.h"
#include "XrGenericTracker.h"
#include "generated/interfaces/vrtypes.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#if defined(SUPPORT_DX)
#include <dxgi.h>
#endif
#if defined(_WIN32) && defined(SUPPORT_DX) && defined(SUPPORT_DX11)
#include <d3d11_4.h>
#endif

#if defined(SUPPORT_GL) && !defined(_WIN32)
#include <GL/glx.h>
#endif

#include <openxr/openxr_platform.h>

// On Android, the app has to pass the OpenGLES setup data through
#ifdef ANDROID
#include "../OpenOVR/Misc/android_api.h"
#endif

// FIXME find a better way to send the OnPostFrame call?
#include "../OpenOVR/Reimpl/BaseApplications.h"
#include "../OpenOVR/Reimpl/BaseClientCore.h"
#include "../OpenOVR/Reimpl/BaseInput.h"
#include "../OpenOVR/Reimpl/BaseOverlay.h"
#include "../OpenOVR/Reimpl/BaseSystem.h"
#include "../OpenOVR/Compositor/DepthRange.h"
#include "../OpenOVR/Misc/Equirect.h"
#include "../OpenOVR/Misc/Config.h"
#include "../OpenOVR/Misc/FrameStats.h"
#include "../OpenOVR/Misc/MirrorWindow.h"
#include "../OpenOVR/convert.h"
#include "generated/static_bases.gen.h"

#include "generated/interfaces/IVRCompositor_018.h"

#include "GfxInterop.h"
#include "tmp_gfx/TemporaryGraphics.h"

#if defined(SUPPORT_VK)
#include "tmp_gfx/TemporaryVk.h"
#endif

#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
#include "tmp_gfx/TemporaryD3D11.h"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

using namespace vr;

std::mutex inputRestartMutex;
std::unique_ptr<TemporaryGraphics> XrBackend::temporaryGraphics = nullptr;
XrBackend::XrBackend(bool useVulkanTmpGfx, bool useD3D11TmpGfx)
{
	memset(projectionViews, 0, sizeof(projectionViews));

	// setup temporaryGraphics

#if defined(SUPPORT_VK)
	if (useVulkanTmpGfx) {
		temporaryGraphics = std::make_unique<TemporaryVk>();
		if (!temporaryGraphics->usable)
			temporaryGraphics.reset(); // D3D11 next
	}
#endif

#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	if (!temporaryGraphics && useD3D11TmpGfx) {
		temporaryGraphics = std::make_unique<TemporaryD3D11>();
		if (!temporaryGraphics->usable)
			temporaryGraphics.reset();
	}
#endif

	// Without one, CreateOpenXRBackend gives up (HasTemporaryGraphics)
	if (!temporaryGraphics)
		OOVR_LOG("No temporary graphics API available for the OpenXR session");

	// setup the device indexes
	for (vr::TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; i++) {
		std::shared_ptr<ITrackedDevice> dev = GetDevice(i);

		if (dev)
			dev->InitialiseDevice(i);
	}
}

XrBackend::~XrBackend()
{
	// Its thread draws from frames this backend reads back
	MirrorWindow::Get().Hide();
	KeepForMirror(vr::Eye_Left, nullptr, nullptr);
	KeepForMirror(vr::Eye_Right, nullptr, nullptr);

	// The whole teardown under the session lock, as every other change to the session is. VR_Shutdown waits for it
	// as long as it takes; process exit without VR_Shutdown waits at most a second (see lock_for_teardown).
	// Not a conditional expression: MSVC 19.44 wants a copy constructor for one, which SessionLock lacks
	auto lock = [] {
		if (BackendManager::IsResetting())
			return xr_session.lock();
		return xr_session.lock_for_teardown(std::chrono::seconds(1));
	}();

	// First clear out the compositors, since they might try and access the OpenXR instance
	// in their destructor.
	PrepareForSessionShutdown();

	// Only here and not in PrepareForSessionShutdown, which also runs when the session is restarted
	if (BaseClientCore::appType == vr::VRApplication_Scene)
		BaseApplications::SetSceneProcess(false);

	DrvOpenXR::FullShutdown();

	graphicsBinding = nullptr;

	// This must happen after session destruction (which occurs in FullShutdown), as runtimes (namely Monado)
	// may try to access these resources while destroying the session.
	temporaryGraphics.reset();
	gfxInterop.reset();
#if defined(SUPPORT_DX) && defined(SUPPORT_DX10) && defined(SUPPORT_DX11)
	DX10Compositor::ReleaseBridgeDevice();
#endif
}

XrSessionState XrBackend::GetSessionState()
{
	return sessionState;
}

std::shared_ptr<IHMD> XrBackend::GetPrimaryHMD()
{
	return hmd;
}

std::shared_ptr<ITrackedDevice> XrBackend::GetDevice(
    vr::TrackedDeviceIndex_t index)
{
	switch (index) {
	case vr::k_unTrackedDeviceIndex_Hmd:
		return GetPrimaryHMD();
	case 1:
		return Hand(XrController::XCT_LEFT);
	case 2:
		return Hand(XrController::XCT_RIGHT);
	default:
		std::shared_lock lock(generic_trackers_mutex);
		vr::TrackedDeviceIndex_t corrected_index = index - RESERVED_DEVICE_INDICES;
		if (corrected_index >= generic_trackers.size())
			return nullptr;

		return generic_trackers.at(corrected_index);
	}
}

std::shared_ptr<ITrackedDevice> XrBackend::GetDeviceByHand(
    ITrackedDevice::TrackedDeviceType hand)
{
	switch (hand) {
	case ITrackedDevice::HAND_LEFT:
		return Hand(XrController::XCT_LEFT);
	case ITrackedDevice::HAND_RIGHT:
		return Hand(XrController::XCT_RIGHT);
	default:
		OOVR_SOFT_ABORTF("Cannot get hand by type '%d'", (int)hand);
		return nullptr;
	}
}

void XrBackend::GetDeviceToAbsoluteTrackingPose(
    vr::ETrackingUniverseOrigin toOrigin,
    float predictedSecondsToPhotonsFromNow,
    vr::TrackedDevicePose_t* poseArray,
    uint32_t poseArrayCount)
{
	for (uint32_t i = 0; i < poseArrayCount; ++i) {
		std::shared_ptr<ITrackedDevice> dev = GetDevice(i);
		if (dev) {
			dev->GetPose(toOrigin, &poseArray[i], ETrackingStateType::TrackingStateType_Rendering);
		} else {
			poseArray[i] = BackendManager::InvalidPose();
		}
	}
}

// False (logged) if desired_queue isn't one of dev's queues
static bool find_queue_family_and_queue_idx(VkDevice dev, VkPhysicalDevice pdev, uint32_t appFamilyIndex, VkQueue desired_queue, uint32_t& out_queueFamilyIndex, uint32_t& out_queueIndex)
{
	if (desired_queue == VK_NULL_HANDLE) {
		OOVR_LOG("The OpenVR app gave no VkQueue with its texture");
		return false;
	}

	uint32_t queue_family_count;
	vkGetPhysicalDeviceQueueFamilyProperties(pdev, &queue_family_count, NULL);

	std::vector<VkQueueFamilyProperties> hi(queue_family_count);
	vkGetPhysicalDeviceQueueFamilyProperties(pdev, &queue_family_count, hi.data());
	OOVR_LOGF("number of queue families is %d", queue_family_count);

	auto searchFamily = [&](uint32_t i) {
		OOVR_LOGF("queue family %d has %d queues", i, hi[i].queueCount);
		for (uint32_t j = 0; j < hi[i].queueCount; j++) {
			VkQueue tmp = VK_NULL_HANDLE;
			vkGetDeviceQueue(dev, i, j, &tmp);
			if (tmp == desired_queue) {
				OOVR_LOGF("Got desired queue: %d %d", i, j);
				out_queueFamilyIndex = i;
				out_queueIndex = j;
				return true;
			}
		}
		return false;
	};

	// Asking for queues of a family the device wasn't created with is invalid, so start with the app's family
	if (appFamilyIndex < queue_family_count && searchFamily(appFamilyIndex))
		return true;
	for (uint32_t i = 0; i < queue_family_count; i++) {
		if (i != appFamilyIndex && searchFamily(i)) {
			OOVR_LOGF("WARNING: The OpenVR app's VkQueue is in queue family %u, not the %u it gave", i, appFamilyIndex);
			return true;
		}
	}
	OOVR_LOG("Couldn't find the queue family index/queue index of the queue that the OpenVR app gave us!"
	         "This is really odd and really shouldn't ever happen");
	return false;
}

#ifdef SUPPORT_VK
/**
 * Whether other is the same GPU as runtimeDevice (a physical device of instance), seen through another VkInstance.
 * Physical device handles differ between instances, so compare the GPU and driver's identity, and only when instance
 * has no second device with that identity that it could be instead.
 */
static bool is_same_gpu(VkInstance instance, VkPhysicalDevice runtimeDevice, VkPhysicalDevice other)
{
	VkPhysicalDeviceProperties want, have;
	vkGetPhysicalDeviceProperties(runtimeDevice, &want);
	vkGetPhysicalDeviceProperties(other, &have);
	if (want.vendorID != have.vendorID || want.deviceID != have.deviceID || want.driverVersion != have.driverVersion
	    || memcmp(want.pipelineCacheUUID, have.pipelineCacheUUID, VK_UUID_SIZE) != 0)
		return false;

	uint32_t count = 0;
	if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS)
		return false;
	std::vector<VkPhysicalDevice> devices(count);
	if (vkEnumeratePhysicalDevices(instance, &count, devices.data()) < 0)
		return false;

	int matching = 0;
	for (uint32_t i = 0; i < count; i++) {
		VkPhysicalDeviceProperties props;
		vkGetPhysicalDeviceProperties(devices[i], &props);
		if (props.vendorID == want.vendorID && props.deviceID == want.deviceID && props.driverVersion == want.driverVersion
		    && memcmp(props.pipelineCacheUUID, want.pipelineCacheUUID, VK_UUID_SIZE) == 0)
			matching++;
	}
	return matching == 1;
}
#endif

#if defined(SUPPORT_DX)
// The name of the DXGI adapter with luid (LowPart | HighPart << 32), for logs
static std::string AdapterName(uint64_t luid)
{
	std::string name = "an unknown adapter";
	IDXGIFactory1* factory = nullptr;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory)
		return name;
	IDXGIAdapter1* adapter = nullptr;
	// Bounded by the adapter count (EnumAdapters1 ends with DXGI_ERROR_NOT_FOUND)
	for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
		DXGI_ADAPTER_DESC1 desc{};
		bool match = SUCCEEDED(adapter->GetDesc1(&desc))
		    && ((uint64_t)desc.AdapterLuid.LowPart | ((uint64_t)(uint32_t)desc.AdapterLuid.HighPart << 32)) == luid;
		adapter->Release();
		if (match) {
			char buffer[256] = {};
			WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, buffer, sizeof(buffer) - 1, nullptr, nullptr);
			name = buffer;
			break;
		}
	}
	factory->Release();
	char ids[40];
	snprintf(ids, sizeof(ids), " (LUID %08x:%08x)", (uint32_t)(luid >> 32), (uint32_t)luid);
	return name + ids;
}

// Whether the game's device, on the adapter gameLuid, is on the OpenXR runtime's adapter (or the runtime names
// none). One soft failure if not: the session is still tried, as a runtime may copy across adapters, and Submit answers
// SharedTexturesNotSupported if it or its swapchains then fail.
static bool OnRuntimeAdapter(uint64_t gameLuid)
{
	uint64_t runtimeLuid = 0;
	if (!BaseSystem::_RuntimeAdapterLuid(&runtimeLuid) || runtimeLuid == gameLuid)
		return true;
	std::string game = AdapterName(gameLuid), runtime = AdapterName(runtimeLuid);
	OC_SOFT_CHECK(false, false, "The game renders on %s, but the OpenXR runtime on %s: trying the session anyway "
	                            "(if it or its swapchains fail, Submit answers SharedTexturesNotSupported)",
	    game.c_str(), runtime.c_str());
	return false;
}

static uint64_t PackLuid(const LUID& luid)
{
	return (uint64_t)luid.LowPart | ((uint64_t)(uint32_t)luid.HighPart << 32);
}
#endif

// VREvent_Quit for the game, as SteamVR sends when its compositor can't go on
static void EnqueueQuit()
{
	VREvent_t quit = { VREvent_Quit };
	auto system = GetBaseSystem();
	if (system)
		system->_EnqueueEvent(quit);
}

/* Submitting Frames */
bool XrBackend::CheckOrInitCompositors(const vr::Texture_t* tex)
{
	// A texture OpenComposite can't use: the eye is left out and Submit answers InvalidTexture
	if (!tex || !tex->handle)
		return false;

	// Check we're using the session with the application's device
	if (!usingApplicationGraphicsAPI) {
		if (appGraphicsUnusable)
			return false; // tried once already: the session stays on the temporary device

		// What can be checked before the session goes down is checked here, so a texture that can't be used
		// leaves the running session alone
		// An OpenGL or Vulkan texture where the runtime lacks that API but has D3D11: copied through D3D11 (GfxInterop)
		std::unique_ptr<GfxInterop> interop;
#if defined(_WIN32) && defined(SUPPORT_DX) && defined(SUPPORT_DX11)
		const bool runtimeHasD3D11 = xr_ext->xrGetD3D11GraphicsRequirementsKHR_Available();
#else
		const bool runtimeHasD3D11 = false;
#endif
		switch (tex->eType) {
		case vr::TextureType_DirectX:
#if !(defined(SUPPORT_DX) && defined(SUPPORT_DX11))
#ifdef __linux__
			OOVR_LOG("Application is trying to submit a D3D11 texture, this is likely a Steam Proton bug, please report it");
#else
			OOVR_LOG("Application is trying to submit a D3D11 texture, which OpenLamina supports but is disabled in this build");
#endif
			appGraphicsUnusable = true;
			return false;
#else
		{
			// The game's device's adapter, D3D10's (DX10Compositor) or D3D11's
			ComPtr<IDXGIDevice> dxgi;
#if defined(SUPPORT_DX10)
			if (ID3D10Texture2D* tex10 = DX10Compositor::AsD3D10Texture(tex)) {
				ComPtr<ID3D10Device> device10;
				tex10->GetDevice(&device10);
				if (device10)
					device10.As(&dxgi);
			} else
#endif
			{
				ComPtr<ID3D11Device> device11;
				((ID3D11Texture2D*)tex->handle)->GetDevice(&device11);
				if (device11)
					device11.As(&dxgi);
			}
			ComPtr<IDXGIAdapter> adapter;
			DXGI_ADAPTER_DESC desc{};
			if (dxgi && SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc)) && !OnRuntimeAdapter(PackLuid(desc.AdapterLuid)))
				appOnOtherAdapter = true;
		}
#endif
			break;
		case vr::TextureType_DirectX12:
#if !(defined(SUPPORT_DX) && defined(SUPPORT_DX12))
#ifdef __linux__
			OOVR_LOG("Application is trying to submit a D3D12 texture, this is likely a Steam Proton bug, please report it");
#else
			OOVR_LOG("Application is trying to submit a D3D12 texture, which OpenLamina supports but is disabled in this build");
#endif
			appGraphicsUnusable = true;
			return false;
#else
			if (!((D3D12TextureData_t*)tex->handle)->m_pResource) {
				OOVR_LOG_ONCE("D3D12 texture data without a resource");
				return false;
			}
			{
				ComPtr<ID3D12Device> device12;
				((D3D12TextureData_t*)tex->handle)->m_pResource->GetDevice(IID_PPV_ARGS(&device12));
				if (device12 && !OnRuntimeAdapter(PackLuid(device12->GetAdapterLuid())))
					appOnOtherAdapter = true;
			}
#endif
			break;
		case vr::TextureType_Vulkan: {
#if !defined(SUPPORT_VK)
			OOVR_LOG("Application is trying to submit a Vulkan texture, which this build doesn't support");
			appGraphicsUnusable = true;
			return false;
#else
			const auto* vktex = (const vr::VRVulkanTextureData_t*)tex->handle;
			if (!vktex->m_pInstance || !vktex->m_pDevice || !vktex->m_pQueue) {
				OOVR_LOG_ONCE("The OpenVR app's Vulkan texture data has no instance, device or queue");
				return false;
			}
			if (!xr_ext->xrGetVulkanGraphicsDeviceKHR_Available() && runtimeHasD3D11) {
				interop = GfxInterop::ForVulkan(vktex->m_pInstance, vktex->m_pPhysicalDevice, vktex->m_pDevice, vktex->m_pQueue);
				if (!interop) {
					OOVR_LOG("The OpenXR runtime has no Vulkan, and the game's Vulkan frames can't go through D3D11");
					appGraphicsUnusable = true;
					return false;
				}
			}
#endif
			break;
		}
		case vr::TextureType_OpenGL:
#if defined(SUPPORT_GL) && defined(_WIN32)
			if (!wglGetCurrentContext() || !wglGetCurrentDC()) {
				OOVR_LOG_ONCEF("Null OpenGL GLRC or DC: %p,%p", (void*)wglGetCurrentContext(), (void*)wglGetCurrentDC());
				return false;
			}
			if (!xr_ext->xrGetOpenGLGraphicsRequirementsKHR_Available() && runtimeHasD3D11) {
				interop = GfxInterop::ForGL();
				if (!interop) {
					OOVR_LOG("The OpenXR runtime has no OpenGL, and the game's OpenGL frames can't go through D3D11");
					appGraphicsUnusable = true;
					return false;
				}
			}
#elif defined(SUPPORT_GL)
			if (!glXGetCurrentDisplay() || !glXGetCurrentContext()) {
				OOVR_LOG_ONCE("No current GLX display or context for the app's OpenGL texture");
				return false;
			}
#elif defined(SUPPORT_GLES)
			if (!OpenComposite_Android_GLES_Binding_Info || OpenComposite_Android_GLES_Binding_Info->type != XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR) {
				OOVR_LOG("App is trying to use GLES, but OpenComposite_Android_GLES_Binding_Info global is not set (properly).\n"
				         "Please ensure this is set by the application.");
				appGraphicsUnusable = true;
				return false;
			}
#else
			OOVR_LOG("Application is trying to submit an OpenGL texture, which OpenLamina supports but is disabled in this build");
			appGraphicsUnusable = true;
			return false;
#endif
			break;
		default:
			OOVR_LOG_ONCEF("Invalid/unknown texture type %d", tex->eType);
			return false;
		}

		// Held through the shutdown, the new session and the fallback. Under Submit's shared lock this waits for the other
		// threads' shared holds to end, and the shared lock is taken back when this ends.
		auto restartLock = xr_session.lock();

		// Another thread may have set the session up on the game's device, or given up on it, while this one waited
		if (appGraphicsUnusable)
			return false;
		if (usingApplicationGraphicsAPI)
			return CheckOrInitCompositors(tex);

		usingApplicationGraphicsAPI = true;

		OOVR_LOG("Recreating OpenXR session for application graphics API");

		// Shutdown old session - apparently Varjo doesn't like the session being destroyed
		// after querying for graphics requirements.
		DrvOpenXR::ShutdownSession();

		// If the game's device can't be bound after all: back to a session on the temporary device, and no more tries.
		// The game can't be seen, so it's told to quit, and its calls keep working on that session while it exits.
		auto giveUp = [&](const char* why) {
			OOVR_LOGF("Can't use the application's graphics device: %s", why);
			graphicsBinding = nullptr;
			usingApplicationGraphicsAPI = false;
			appGraphicsUnusable = true;
			if (!temporaryGraphics || XR_FAILED(DrvOpenXR::SetupSession())) {
				OnSessionLost(); // not even the temporary device gets a session: OnSessionLost tells the game to quit
			} else {
				OOVR_LOG("The game's frames can't be shown: telling the game to quit");
				EnqueueQuit();
			}
			return false;
		};

#if defined(_WIN32) && defined(SUPPORT_DX) && defined(SUPPORT_DX11)
		if (interop) {
			// The session is D3D11, on the interop's device on the runtime's adapter
			XrGraphicsRequirementsD3D11KHR graphicsRequirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
			OC_XR_LOG(xr_ext->xrGetD3D11GraphicsRequirementsKHR(xr_instance, xr_system, &graphicsRequirements));
			XrGraphicsBindingD3D11KHR d3dInfo{ XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
			d3dInfo.device = interop->Device();
			graphicsBinding = std::make_unique<BindingWrapper<XrGraphicsBindingD3D11KHR>>(d3dInfo);
			// Kept with the binding, for each later session too
			gfxInterop = std::move(interop);
			if (XR_FAILED(DrvOpenXR::SetupSession()))
				return giveUp("the OpenXR runtime refused a D3D11 session for the game's copied frames");
		}
#endif
		// The game's own API's session (none here for frames copied through D3D11, bound above)
		switch (gfxInterop ? vr::TextureType_Invalid : tex->eType) {
#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
		case vr::TextureType_DirectX: {
			// The spec requires that we call this before starting a session using D3D. Unfortunately we
			// can't actually do anything with this information, since the game has already created the device.
			XrGraphicsRequirementsD3D11KHR graphicsRequirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
			OC_XR_LOG(xr_ext->xrGetD3D11GraphicsRequirementsKHR(xr_instance, xr_system, &graphicsRequirements));

			ID3D11Device* dev = nullptr;
#if defined(SUPPORT_DX10)
			if (ID3D10Texture2D* tex10 = DX10Compositor::AsD3D10Texture(tex)) {
				// A Direct3D 10 texture: the session is on a D3D11 device of its device's adapter, and each frame is
				// copied across (DX10Compositor)
				dev = DX10Compositor::BridgeDevice(tex10);
				if (!dev)
					return giveUp("no Direct3D 11 device on the Direct3D 10 device's adapter");
				dev->AddRef();
			} else
#endif
			{
				auto* d3dTex = (ID3D11Texture2D*)tex->handle;
				d3dTex->GetDevice(&dev);
			}
			if (!dev)
				return giveUp("the D3D11 texture has no device");

			XrGraphicsBindingD3D11KHR d3dInfo{};
			d3dInfo.type = XR_TYPE_GRAPHICS_BINDING_D3D11_KHR;
			d3dInfo.device = dev;
			graphicsBinding = std::make_unique<BindingWrapper<XrGraphicsBindingD3D11KHR>>(d3dInfo);
			XrResult made = DrvOpenXR::SetupSession();

			dev->Release();
			if (XR_FAILED(made))
				return giveUp("the OpenXR runtime refused a session on the game's D3D11 device");
			break;
		}
#endif
#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)
		case vr::TextureType_DirectX12: {
			// The spec requires that we call this before starting a session using D3D. Unfortunately we
			// can't actually do anything with this information, since the game has already created the device.
			XrGraphicsRequirementsD3D12KHR graphicsRequirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR };
			OC_XR_LOG(xr_ext->xrGetD3D12GraphicsRequirementsKHR(xr_instance, xr_system, &graphicsRequirements));

			D3D12TextureData_t* d3dTexData = (D3D12TextureData_t*)tex->handle;
			ComPtr<ID3D12Device> device;
			d3dTexData->m_pResource->GetDevice(IID_PPV_ARGS(&device));
			if (!device)
				return giveUp("the D3D12 resource has no device");

			XrGraphicsBindingD3D12KHR d3dInfo{};
			d3dInfo.type = XR_TYPE_GRAPHICS_BINDING_D3D12_KHR;
			d3dInfo.device = device.Get();
			d3dInfo.queue = d3dTexData->m_pCommandQueue;
			graphicsBinding = std::make_unique<BindingWrapper<XrGraphicsBindingD3D12KHR>>(d3dInfo);
			if (XR_FAILED(DrvOpenXR::SetupSession()))
				return giveUp("the OpenXR runtime refused a session on the game's D3D12 device");

#ifdef _DEBUG
			ComPtr<ID3D12Debug> debugController;
			D3D12GetDebugInterface(IID_PPV_ARGS(&debugController));
			debugController->EnableDebugLayer();
#endif
			// The ComPtr releases the device: no explicit Release here
			break;
		}
#endif
#if defined(SUPPORT_VK)
		case vr::TextureType_Vulkan: {
			const vr::VRVulkanTextureData_t* vktex = (vr::VRVulkanTextureData_t*)tex->handle;

			// The spec requires that we call this before starting a session using Vulkan (a runtime may refuse the
			// session without it). Unfortunately we can't actually do anything with this information, since the game has
			// already created the device.
			XrGraphicsRequirementsVulkanKHR graphicsRequirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR };
			OC_XR_LOG(xr_ext->xrGetVulkanGraphicsRequirementsKHR(xr_instance, xr_system, &graphicsRequirements));

			VkPhysicalDevice xr_desire = VK_NULL_HANDLE;
			// We have to call this, or we get crazy validation errors
			XrResult desireResult = xr_ext->xrGetVulkanGraphicsDeviceKHR(xr_instance, xr_system, vktex->m_pInstance, &xr_desire);
			if (XR_FAILED(desireResult) || xr_desire == VK_NULL_HANDLE)
				return giveUp("xrGetVulkanGraphicsDeviceKHR failed for the OpenVR app's VkInstance");

			// XR_KHR_vulkan_enable requires the binding's physical device to be the one the runtime returned (Monado
			// checks). No Man's Sky under Wine passes a physical device from another of its instances than m_pInstance:
			// if that's the same GPU, use the runtime's handle for it.
			VkPhysicalDevice physicalDevice = vktex->m_pPhysicalDevice;
			if (xr_desire != physicalDevice) {
				if (physicalDevice == VK_NULL_HANDLE || !is_same_gpu(vktex->m_pInstance, xr_desire, physicalDevice)) {
					OOVR_LOGF("The VkPhysicalDevice that the OpenVR app (%p) used is different from the one that the OpenXR runtime used (%p)!\n"
					          "This should never happen, except for on multi-gpu, in which case DRI_PRIME=1 should fix things on Linux.",
					    (void*)vktex->m_pPhysicalDevice, (void*)xr_desire);
					return giveUp("the app's VkPhysicalDevice isn't the OpenXR runtime's");
				}
				OOVR_LOGF("WARNING: The OpenVR app's VkPhysicalDevice (%p) isn't the runtime's (%p) for its instance, but it's the same GPU: "
				          "using the runtime's",
				    (void*)vktex->m_pPhysicalDevice, (void*)xr_desire);
				physicalDevice = xr_desire;
			}

			XrGraphicsBindingVulkanKHR binding;
			binding.type = XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR;
			binding.next = nullptr;
			binding.instance = vktex->m_pInstance;
			binding.physicalDevice = physicalDevice;
			binding.device = vktex->m_pDevice;

			if (!find_queue_family_and_queue_idx( //
			        binding.device, //
			        binding.physicalDevice, //
			        vktex->m_nQueueFamilyIndex, //
			        vktex->m_pQueue, //
			        binding.queueFamilyIndex, //
			        binding.queueIndex //
			        ))
				return giveUp("the app's VkQueue isn't one of its device's queues");

			graphicsBinding = std::make_unique<BindingWrapper<XrGraphicsBindingVulkanKHR>>(binding);
			if (XR_FAILED(DrvOpenXR::SetupSession()))
				return giveUp("the OpenXR runtime refused a session on the game's Vulkan device");
			break;
		}
#endif
		case vr::TextureType_OpenGL: {
#ifdef SUPPORT_GL
			// The spec requires that we call this before starting a session using OpenGL. Unfortunately we
			// can't actually do anything with this information, since the game has already created the context.
			XrGraphicsRequirementsOpenGLKHR graphicsRequirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
			OC_XR_LOG(xr_ext->xrGetOpenGLGraphicsRequirementsKHR(xr_instance, xr_system, &graphicsRequirements));

			// Platform-specific OpenGL context stuff:
#ifdef _WIN32
			XrGraphicsBindingOpenGLWin32KHR binding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR };
			binding.hGLRC = wglGetCurrentContext();
			binding.hDC = wglGetCurrentDC();

			graphicsBinding = std::make_unique<BindingWrapper<XrGraphicsBindingOpenGLWin32KHR>>(binding);
			if (XR_FAILED(DrvOpenXR::SetupSession()))
				return giveUp("the OpenXR runtime refused a session on the game's OpenGL context");
#else
			// Only support xlib for now (same as Monado)
			// TODO wayland
			// TODO xcb

			// HACK: Create a separate GLX context since `MaybeRestartForInputs()` may attempt to bind it on a different thread while the app's context is still in use
			const PFNGLXCREATECONTEXTATTRIBSARBPROC pfn_glXCreateContextAttribsARB = (PFNGLXCREATECONTEXTATTRIBSARBPROC)glXGetProcAddress((const GLubyte*)"glXCreateContextAttribsARB");
			if (pfn_glXCreateContextAttribsARB == nullptr)
				return giveUp("glXCreateContextAttribsARB not available");

			XrGraphicsBindingOpenGLXlibKHR binding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR };
			binding.xDisplay = glXGetCurrentDisplay();
			int configCount = 0;
			// clang-format off
			const int configAttribs[] = {
				GLX_CONFIG_CAVEAT, GLX_NONE,
				None,
			};
			// clang-format on
			GLXFBConfig* const configs = glXChooseFBConfig(binding.xDisplay, DefaultScreen(binding.xDisplay), configAttribs, &configCount);
			if (configs == nullptr || configCount < 1)
				return giveUp("No usable GLXFBConfig found");
			binding.glxFBConfig = configs[0];
			XFree(configs);
			XVisualInfo* visual = glXGetVisualFromFBConfig(binding.xDisplay, binding.glxFBConfig);
			if (!visual)
				return giveUp("No visual for the GLXFBConfig");
			binding.visualid = visual->visualid;
			XFree(visual);
			binding.glxDrawable = glXGetCurrentDrawable();
			// clang-format off
			const int contextAttribs[] = {
				GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
				GLX_CONTEXT_MINOR_VERSION_ARB, 2,
				None,
			};
			// clang-format on
			binding.glxContext = pfn_glXCreateContextAttribsARB(binding.xDisplay, binding.glxFBConfig, glXGetCurrentContext(), true, contextAttribs);
			if (!binding.glxContext)
				return giveUp("glXCreateContextAttribsARB failed");

			graphicsBinding = std::make_unique<BindingWrapper<XrGraphicsBindingOpenGLXlibKHR>>(binding);
			if (XR_FAILED(DrvOpenXR::SetupSession()))
				return giveUp("the OpenXR runtime refused a session on the game's OpenGL context");
#endif
			// End of platform-specific code

#elif defined(SUPPORT_GLES)
			// The spec requires that we call this before starting a session using OpenGL. We could actually handle this properly
			// on android since the app has to be modified to work with us, but for now don't bother.
			XrGraphicsRequirementsOpenGLESKHR graphicsRequirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
			OC_XR_LOG(xr_ext->xrGetOpenGLESGraphicsRequirementsKHR(xr_instance, xr_system, &graphicsRequirements));

			XrGraphicsBindingOpenGLESAndroidKHR binding = *OpenComposite_Android_GLES_Binding_Info;
			binding.next = nullptr;

			graphicsBinding = std::make_unique<BindingWrapper<XrGraphicsBindingOpenGLESAndroidKHR>>(binding);
			if (XR_FAILED(DrvOpenXR::SetupSession()))
				return giveUp("the OpenXR runtime refused a session on the game's OpenGL ES context");
#endif
			break;
		}
		default:
			// Checked above
			break;
		}

		// Real graphics binding should be setup now - get rid of temporary graphics
		temporaryGraphics.reset();
	}

	// Copied textures' compositors are made from their D3D11 copies (StoreEyeTexture)
	if (gfxInterop && tex->eType == gfxInterop->Type())
		return true;

	bool ready = true;
	for (std::unique_ptr<Compositor>& compositor : compositors) {
		// Skip a compositor if it's already set up
		if (!compositor)
			compositor = BaseCompositor::CreateCompositorAPI(tex);
		ready = ready && compositor != nullptr;
	}
	return ready;
}

void XrBackend::WaitForTrackingData()
{
	// Make sure the OpenXR session is active before doing anything else, and if not then skip
	if (!sessionActive) {
		renderingFrame = false;
		// No xrWaitFrame to pace the game: about one frame's wait instead of a busy loop
		std::this_thread::sleep_for(std::chrono::milliseconds(11));
		return;
	}

	XrFrameWaitInfo waitInfo{ XR_TYPE_FRAME_WAIT_INFO };
	XrFrameState state{ XR_TYPE_FRAME_STATE };

	// Held to the end: a session restart on another thread replaces xr_gbl
	auto lock = xr_session.lock_shared();

	// Checked again under the lock: a restart this thread waited behind may have left no running session
	if (!sessionActive) {
		renderingFrame = false;
		return;
	}

	// The game's frame from here to its xrEndFrame (SubmitFrames): an own frame in flight is ended first
	std::unique_lock<std::mutex> owner(frameOwner);
	if (ownFrames)
		ownFrames->OnAppFrameStart();

	// A waited frame not yet begun is begun now, with its predicted display time
	if (!frameAwaitingBegin) {
		auto waitCall = std::chrono::steady_clock::now();
		if (XrResult res = xrWaitFrame(xr_session.get(), &waitInfo, &state); XR_FAILED(res)) {
			// This frame is skipped: no xrBeginFrame, so no xrEndFrame; poses are the last ones
			OOVR_LOG_ONCE("xrWaitFrame failed: skipping frames until it works again");
			OnFrameCallFailed(res);
			return;
		}
		auto waitReturned = std::chrono::steady_clock::now();
		RecordFrameTiming(waitCall, waitReturned, state.predictedDisplayTime, state.predictedDisplayPeriod);
		xr_gbl->nextPredictedFrameTime = state.predictedDisplayTime;
		frameAwaitingBegin = true;

		FrameStats::Get().OnFrameStart();
		OOVR_Compositor_FrameTiming timing{};
		timing.m_nSize = sizeof(timing);
		if (GetFrameTiming(&timing, 0))
			FrameStats::Get().OnAppFrame(timing, measuredDropped, waitReturned, displayPeriodMs);
	}

	// FIXME loop until this returns true?
	// OOVR_FALSE_ABORT(state.shouldRender);

	XrFrameBeginInfo beginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
	if (XrResult res = xrBeginFrame(xr_session.get(), &beginInfo); XR_FAILED(res)) {
		// No xrEndFrame after a failed xrBeginFrame: this frame is dropped, and the next one begins it again
		// (or, if the runtime says there's no waited frame, waits for a new one)
		OOVR_LOG_ONCE("xrBeginFrame failed: dropping frames until it works again");
		if (res == XR_ERROR_CALL_ORDER_INVALID)
			frameAwaitingBegin = false;
		OnFrameCallFailed(res);
		return;
	}
	frameAwaitingBegin = false;
	frameBegun = true;
	frameCallFailures = 0;
	owner.unlock();

	xr_gbl->ClearCachedViews();

	XrSpace projectionSpace = xr_space_from_ref_space_type(BaseSystem::currentSpace);
	const XruCachedViews& cachedViews = xr_gbl->GetCachedViews(projectionSpace);
	const XrViewState& viewState = cachedViews.viewState;
	const std::array<XrView, XruEyeCount>& views = cachedViews.views;

	for (int eye = 0; eye < XruEyeCount; eye++) {
		projectionViews[eye].fov = views[eye].fov;

		XrPosef pose = views[eye].pose;

		// Make sure we at least have halfway-sane values if the runtime isn't providing them. In particular
		// if the runtime gives us an invalid orientation, that'd otherwise cause XR_ERROR_POSE_INVALID errors later.
		if ((viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0) {
			pose.orientation = XrQuaternionf{ 0, 0, 0, 1 };
		}
		if ((viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) == 0) {
			// About 1.75m up above the origin
			pose.position = XrVector3f{ 0, 1.75, 0 };
		}

		projectionViews[eye].pose = pose;
		// No depth until this frame's texture brings some
		projectionViews[eye].next = nullptr;
	}

	// If we're not on the game's graphics API yet, don't actually mark us as having started the frame.
	// Instead, set a different flag so we'll call this method again when it's available.
	if (!usingApplicationGraphicsAPI) {
		deferredRenderingStart = true;
	} else {
		renderingFrame = true;
	}
}

// EVRCompositorError values (the same in every SDK version) for what StoreEyeTexture answers Submit
static constexpr IBackend::openvr_enum_t kCompositorNone = 0;
static constexpr IBackend::openvr_enum_t kCompositorRequestFailed = 1;
static constexpr IBackend::openvr_enum_t kCompositorInvalidTexture = 102;
static constexpr IBackend::openvr_enum_t kCompositorUnsupportedFormat = 105;
static constexpr IBackend::openvr_enum_t kCompositorSharedTexturesNotSupported = 106;

IBackend::openvr_enum_t XrBackend::StoreEyeTexture(
    vr::EVREye eye,
    const vr::Texture_t* texture,
    const vr::VRTextureBounds_t* bounds,
    vr::EVRSubmitFlags submitFlags,
    bool isFirstEye)
{
	if (eye != vr::Eye_Left && eye != vr::Eye_Right)
		return kCompositorInvalidTexture;

	if (!CheckOrInitCompositors(texture)) {
		// A texture OpenComposite can't use (logged): this eye is left out of the frame
		projectionViews[eye].subImage.swapchain = XR_NULL_HANDLE;
		return appOnOtherAdapter && appGraphicsUnusable ? kCompositorSharedTexturesNotSupported : kCompositorInvalidTexture;
	}

	XrCompositionLayerProjectionView& layer = projectionViews[eye];
	layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;

	// An OpenGL or Vulkan texture copied through D3D11: from here on, its D3D11 copy. The game's own texture stays
	// what the pose and depth are read from.
	const vr::Texture_t* submitted = texture;
#if defined(_WIN32) && defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	vr::Texture_t copied;
	vr::VRTextureBounds_t copiedBounds;
	if (gfxInterop && texture->eType == gfxInterop->Type()) {
		ID3D11Texture2D* copy = gfxInterop->CopyIn(texture, bounds);
		if (!copy) {
			layer.subImage.swapchain = XR_NULL_HANDLE;
			return kCompositorRequestFailed;
		}
		// An OpenGL texture's rows run bottom-up and its copy's top-down: the bounds' v, which counted from the bottom,
		// counts from the top of the copy
		if (bounds && texture->eType == vr::TextureType_OpenGL) {
			copiedBounds = { bounds->uMin, 1 - bounds->vMax, bounds->uMax, 1 - bounds->vMin };
			bounds = &copiedBounds;
		}
		copied = { copy, vr::TextureType_DirectX, texture->eColorSpace };
		texture = &copied;
		// A D3D11 compositor for the interop's texture
		if (!compositors[eye])
			compositors[eye] = std::make_unique<DX11Compositor>(copy);
	}
#endif

	std::unique_ptr<Compositor>& compPtr = compositors[eye];
	if (!compPtr) {
		// No compositor for this texture (its graphics API couldn't be set up): this eye is left out
		layer.subImage.swapchain = XR_NULL_HANDLE;
		return kCompositorRequestFailed;
	}
	Compositor& comp = *compPtr;

	// If the session is inactive, we may be unable to write to the surface
	IBackend::openvr_enum_t result = kCompositorNone;
	if (sessionActive && renderingFrame) {
		comp.Invoke(texture, bounds, layer.subImage, (XruEye)eye, submitFlags);
		UseSubmittedPose(eye, submitted, submitFlags);
		// Depth goes only straight from a game's texture: not through the interop's copy, nor D3D10's bridge
		bool depthUsable = texture == submitted;
#if defined(SUPPORT_DX) && defined(SUPPORT_DX10) && defined(SUPPORT_DX11)
		if (DX10Compositor::AsD3D10Texture(submitted))
			depthUsable = false;
#endif
		if (depthUsable)
			SubmitDepth(eye, submitted, bounds, submitFlags);
		else if (submitFlags & vr::Submit_TextureWithDepth)
			OOVR_LOG_ONCE("Depth isn't sent for textures copied through another graphics API");
		XrResult res = comp.LastResult();
		if (res == XR_ERROR_VALIDATION_FAILURE)
			result = kCompositorInvalidTexture; // the compositor's word for texture data it can't use
		else if (XR_FAILED(res) && appOnOtherAdapter)
			result = kCompositorSharedTexturesNotSupported; // a swapchain call failed for a device on another adapter
		else if (res == XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED)
			result = kCompositorUnsupportedFormat;
		else if (XR_FAILED(res))
			result = kCompositorRequestFailed;

		if (result == kCompositorNone) {
			KeepForMirror(eye, texture, bounds);
			// The mirror window shows the left eye, read back while it's open
			Image mirror;
			if (eye == vr::Eye_Left && MirrorWindow::Get().WantsFrame() && comp.ReadbackToCpu(texture, bounds, mirror))
				MirrorWindow::Get().Offer(std::move(mirror));
		}
	}

	submittedEyeTextures = true;

	// TODO store view somewhere and use it for submitting our frame

	// If WaitGetPoses was called before the first texture was submitted, we're in a kinda weird state
	// The application will expect it can submit it's frames (and we do too) however xrBeginFrame was
	// never called for this session - it was called for the early session, then when the first texture
	// was published we switched to that, but this new session hasn't had xrBeginFrame called yet.
	// To get around this, we set a flag if we should begin a frame but are still in the early session. At
	// this point we can check for that flag and call xrBeginFrame a second time, on the right session.
	if (deferredRenderingStart && usingApplicationGraphicsAPI) {
		deferredRenderingStart = false;
		WaitForTrackingData();
	}
	return result;
}

void XrBackend::UseSubmittedPose(vr::EVREye eye, const vr::Texture_t* texture, vr::EVRSubmitFlags submitFlags)
{
	if (!oovr_global_configuration.UseSubmittedPose() || !(submitFlags & vr::Submit_TextureWithPose) || !xr_gbl)
		return;
	// The head pose the game rendered with, in its tracking space: the projection layer's space
	const vr::HmdMatrix34_t& m = ((const vr::VRTextureWithPose_t*)texture)->mDeviceToAbsoluteTracking;
	glm::mat3 rotation(S2G_m34(m));
	glm::quat q = glm::quat_cast(rotation);
	const glm::vec3 position(m.m[0][3], m.m[1][3], m.m[2][3]);
	const float length = glm::length(q);
	if (!(length > 0.5f && length < 1.5f) || !std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) {
		OOVR_LOG_ONCE("A submitted pose that isn't a pose: the located one is used");
		return;
	}
	q = glm::normalize(q);
	// The eye's pose in the head, from the frame's views located in the view space
	const XruCachedViews inHead = xr_gbl->GetCachedViews(xr_gbl->viewSpace);
	const XrViewStateFlags valid = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
	if ((inHead.viewState.viewStateFlags & valid) != valid)
		return;
	const XrPosef& eyePose = inHead.views[eye].pose;
	XrPosef& pose = projectionViews[eye].pose;
	pose.orientation = G2X_quat(glm::normalize(q * X2G_quat(eyePose.orientation)));
	pose.position = G2X_v3f(position + q * X2G_v3f(eyePose.position));
}

void XrBackend::SubmitDepth(vr::EVREye eye, const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds, vr::EVRSubmitFlags submitFlags)
{
	if (!oovr_global_configuration.SubmitDepth() || !(submitFlags & vr::Submit_TextureWithDepth))
		return;
	if (!xr_ext->DepthKHR_Available()) {
		OOVR_LOG_ONCE("The game sends depth, but the runtime has no XR_KHR_composition_layer_depth: not sent");
		return;
	}
	// With a pose too, the depth comes after it
	const vr::VRTextureDepthInfo_t& info = (submitFlags & vr::Submit_TextureWithPose) ? ((const vr::VRTextureWithPoseAndDepth_t*)texture)->depth
	                                                                                 : ((const vr::VRTextureWithDepth_t*)texture)->depth;
	if (!info.handle)
		return;
	float nearZ = 0, farZ = 0;
	if (!DepthRangeFromProjection(info.mProjection, nearZ, farZ)) {
		OOVR_LOG_ONCE("A submitted depth's projection isn't a perspective one: depth not sent");
		return;
	}
	vr::Texture_t depth = { info.handle, texture->eType, vr::ColorSpace_Linear };
#if defined(SUPPORT_VK)
	// A Vulkan depth image is on the colour image's device: some games fill only the image fields of its struct
	vr::VRVulkanTextureData_t vkDepth;
	if (texture->eType == vr::TextureType_Vulkan && texture->handle) {
		const auto* colour = (const vr::VRVulkanTextureData_t*)texture->handle;
		vkDepth = *(const vr::VRVulkanTextureData_t*)info.handle;
		vkDepth.m_pDevice = colour->m_pDevice;
		vkDepth.m_pPhysicalDevice = colour->m_pPhysicalDevice;
		vkDepth.m_pInstance = colour->m_pInstance;
		vkDepth.m_pQueue = colour->m_pQueue;
		vkDepth.m_nQueueFamilyIndex = colour->m_nQueueFamilyIndex;
		depth.handle = &vkDepth;
	}
#endif
	std::unique_ptr<Compositor>& comp = depthCompositors[eye];
	if (!comp)
		comp = BaseCompositor::CreateCompositorAPI(&depth);
	XrSwapchainSubImage sub{};
	if (!comp || !comp->InvokeDepth(&depth, bounds, sub))
		return;
	// The range of depth values used: 0..1 unless the game says otherwise
	float minDepth = info.vRange.v[0], maxDepth = info.vRange.v[1];
	if (!(minDepth >= 0 && maxDepth <= 1 && minDepth < maxDepth)) {
		minDepth = 0;
		maxDepth = 1;
	}
	XrCompositionLayerDepthInfoKHR& d = depthInfos[eye];
	d = { XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR };
	d.subImage = sub;
	d.minDepth = minDepth;
	d.maxDepth = maxDepth;
	d.nearZ = nearZ;
	d.farZ = farZ;
	projectionViews[eye].next = &d;
}

void XrBackend::SubmitFrames(bool showSkybox, bool postPresent)
{
	// Always pump events, even if the session isn't active - this is what makes the session active
	// in the first place.
	PumpEvents();

	// If we are getting calls from PostPresentHandOff then skip the calls from other functions as
	//  there will be other data such as GUI layers to be added before ending the frame.
	bool skipRender = postPresentStatus && !postPresent;
	postPresentStatus = postPresent;

	// Through this frame's xrEndFrame (see frameOwner)
	std::unique_lock<std::mutex> owner(frameOwner);

	if (!renderingFrame || skipRender)
		return;

	// All data submitted, rendering has finished, frame can be ended.
	renderingFrame = false;

	// Make sure the OpenXR session is active before doing anything else
	// Note that if the session becomes ready after WaitGetTrackingPoses was called, then
	// renderingFrame will still be false so this won't be a problem in that case.
	if (!sessionActive)
		return;

	XrFrameEndInfo info{ XR_TYPE_FRAME_END_INFO };
	info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	info.displayTime = xr_gbl->nextPredictedFrameTime;

	XrCompositionLayerBaseHeader const* const* headers = nullptr;
	XrCompositionLayerBaseHeader* app_layer = nullptr;

	int layer_count = 0;

	// Apps can use layers to provide GUIs and loading screens where a 3D environment is not being rendered.
	// Only create the projection layer if we have a 3D environment to submit.
	XrCompositionLayerProjection mainLayer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	if (submittedEyeTextures) {
		// We have eye textures so setup a projection layer
		mainLayer.space = xr_space_from_ref_space_type(BaseSystem::currentSpace);
		mainLayer.views = projectionViews;
		mainLayer.viewCount = 2;

		app_layer = (XrCompositionLayerBaseHeader*)&mainLayer;
		for (uint32_t i = 0; i < mainLayer.viewCount; ++i) {
			XrCompositionLayerProjectionView& layer = projectionViews[i];
			layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
			if (layer.subImage.swapchain == XR_NULL_HANDLE)
				app_layer = nullptr;
		}
		submittedEyeTextures = false;
		if (app_layer) {
			// Kept for OpenLamina's own frames: its swapchains' last images and its views stay valid to submit again
			lastProjection = mainLayer;
			for (uint32_t i = 0; i < 2; i++) {
				lastProjectionViews[i] = projectionViews[i];
				lastProjectionViews[i].next = nullptr;
			}
			lastProjection.views = lastProjectionViews;
			haveLastProjection = true;
		}
	}

	// If we have an overlay then add. Its layers stay as they are until the frame has ended.
	BaseOverlay* overlay = GetUnsafeBaseOverlay();
	std::unique_lock<std::recursive_mutex> overlayLock;
	if (overlay) {
		// The controllers' lasers on the overlays and the keyboard, on this thread: it fills their swapchains
		overlay->_UpdatePointer();
		overlayLock = std::unique_lock(overlay->_LayersLock());
		layer_count = overlay->_BuildLayers(app_layer, headers, true);
	} else if (app_layer) {
		layer_count = 1;
		headers = &app_layer;
	}

	// It's ok if no layers have been added at this point,
	// it will just cause the display to be blanked
	info.layers = headers;
	info.layerCount = layer_count;

	XrResult endResult = xrEndFrame(xr_session.get(), &info);
	OOVR_FAILED_XR_SOFT_ABORT(endResult);
	// A present: the game's eye textures shown
	if (XR_SUCCEEDED(endResult) && app_layer)
		FrameStats::Get().OnAppPresent();
	frameBegun = false;
	if (overlayLock.owns_lock())
		overlayLock.unlock();
	if (ownFrames)
		ownFrames->OnAppFrameEnd();
	owner.unlock();

	BaseSystem* sys = GetUnsafeBaseSystem();
	if (sys) {
		sys->_OnPostFrame();
	}

	auto now = std::chrono::system_clock::now().time_since_epoch();
	frameSubmitTimeUs = (double)std::chrono::duration_cast<std::chrono::microseconds>(now).count() / 1000000.0;

	nFrameIndex++;
}

bool XrBackend::SkyboxShown()
{
	std::lock_guard<std::mutex> lock(skyboxLock);
	return skybox.layerCount > 0;
}

int XrBackend::SetEquirectSkybox(const vr::Texture_t* textures, uint32_t count)
{
	// Six faces are read back and turned into one lat-long image first, without any lock: it takes a while
	Image equirect;
	if (count == 6) {
		if (!skyboxReader)
			skyboxReader = BaseCompositor::CreateCompositorAPI(&textures[0]);
		if (!skyboxReader)
			return 1; // VRCompositorError_RequestFailed
		std::array<Image, 6> faces;
		for (int i = 0; i < 6; i++) {
			if (!skyboxReader->ReadbackToCpu(&textures[i], nullptr, faces[i])) {
				OOVR_LOG_ONCE("SetSkyboxOverride: a cube face couldn't be read back");
				return 1; // VRCompositorError_RequestFailed
			}
		}
		equirect = CubeFacesToEquirect({ &faces[0], &faces[1], &faces[2], &faces[3], &faces[4], &faces[5] },
		    std::min<uint32_t>(2048, faces[0].width * 4));
		if (equirect.Empty())
			return 1; // VRCompositorError_RequestFailed
	}

	XrCompositionLayerEquirect2KHR layer{ XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR };
	layer.space = xr_space_from_ref_space_type(BaseSystem::currentSpace);
	layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	layer.pose = { { 0, 0, 0, 1 }, { 0, 0, 0 } };
	layer.radius = 0; // a sphere at infinity
	constexpr float pi = 3.14159265f;
	layer.centralHorizontalAngle = 2 * pi;
	layer.upperVerticalAngle = pi / 2;
	layer.lowerVerticalAngle = -pi / 2;

	// The swapchains are refilled, and maybe remade, while no own frame uses them
	std::lock_guard<std::mutex> owner(frameOwner);
	std::lock_guard<std::mutex> lock(skyboxLock);
	skybox.layerCount = 0;
	if (count == 6) {
		if (!skybox.faces)
			skybox.faces = CpuSwapchain::Create(equirect.width, equirect.height);
		if (!skybox.faces || !skybox.faces->Upload(equirect))
			return 1; // VRCompositorError_RequestFailed
		layer.subImage = skybox.faces->SubImage();
		skybox.layers[0] = layer;
		skybox.layerCount = 1;
		return 0;
	}

	// One lat-long texture, or a stereo pair: one layer for each eye
	for (uint32_t i = 0; i < count; i++) {
		if (!skybox.textures[i])
			skybox.textures[i] = BaseCompositor::CreateCompositorAPI(&textures[i]);
		if (!skybox.textures[i])
			return 1; // VRCompositorError_RequestFailed
		skybox.layers[i] = layer;
		skybox.textures[i]->Invoke(&textures[i], nullptr, skybox.layers[i].subImage);
		if (XR_FAILED(skybox.textures[i]->LastResult()))
			return 1; // VRCompositorError_RequestFailed
		if (count == 2)
			skybox.layers[i].eyeVisibility = i == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT;
	}
	skybox.layerCount = (int)count;
	return 0;
}

IBackend::openvr_enum_t XrBackend::SetSkyboxOverride(const vr::Texture_t* pTextures, uint32_t unTextureCount)
{
	// With equirect layers: one lat-long texture, a stereo pair or six cube faces, held and shown in OpenLamina's own
	// frames while the game runs none (as SteamVR shows it), until ClearSkyboxOverride
	if (ownFrames && xr_ext && xr_ext->Equirect2KHR_Available() && pTextures && (unTextureCount == 1 || unTextureCount == 2 || unTextureCount == 6)) {
		if (!CheckOrInitCompositors(pTextures))
			return 1; // VRCompositorError_RequestFailed
		auto lock = xr_session.lock_shared();
		if (!sessionActive || !usingApplicationGraphicsAPI)
			return 0;
		return SetEquirectSkybox(pTextures, unTextureCount);
	}

	// Otherwise one texture (or the first of a stereo pair), shown at once as a quad (rFactor2's loading screens). Six
	// faces are a cube map: one face as a flat picture would be wrong, so that's RequestFailed.
	if (unTextureCount >= 6) {
		OOVR_LOG_ONCE("SetSkyboxOverride with a cube map: not supported yet");
		return 1; // VRCompositorError_RequestFailed
	}
	if (unTextureCount && pTextures) {
		if (!CheckOrInitCompositors(pTextures))
			return 1; // VRCompositorError_RequestFailed

		if (!sessionActive || !usingApplicationGraphicsAPI)
			return 0;

		// Taken after CheckOrInitCompositors, which may restart the session, and held to the end: a session restart
		// on another thread replaces xr_gbl
		auto lock = xr_session.lock_shared();
		if (!sessionActive)
			return 0; // a restart this thread waited behind left no running session

		// This frame is run here, from its xrWaitFrame to its xrEndFrame (see frameOwner)
		std::unique_lock<std::mutex> owner(frameOwner);
		if (ownFrames)
			ownFrames->OnAppFrameStart();

		// Make sure any unfinished frames don't call xrEndFrame after this call
		renderingFrame = false;

		XrFrameWaitInfo waitInfo{ XR_TYPE_FRAME_WAIT_INFO };
		XrFrameState state{ XR_TYPE_FRAME_STATE };

		// A waited frame not yet begun (WaitForTrackingData's) is begun here instead
		if (!frameAwaitingBegin) {
			if (XrResult res = xrWaitFrame(xr_session.get(), &waitInfo, &state); XR_FAILED(res)) {
				OOVR_LOG_ONCE("SetSkyboxOverride: xrWaitFrame failed");
				OnFrameCallFailed(res);
				return 1; // VRCompositorError_RequestFailed
			}
			xr_gbl->nextPredictedFrameTime = state.predictedDisplayTime;
			frameAwaitingBegin = true;
		}

		// Not one of the app's frames, so its display slot isn't a dropped frame
		RestartFrameTiming();

		// This submits a frame when a skybox override is set. This is designed around rFactor2 where the skybox is used as
		// a loading screen and is frequently updated, and most other games probably behave in a similar manner. It'd be
		// ideal to run a separate thread while the skybox override is set to submit frames if IVRCompositor->Submit is not
		// being called frequently enough, and that'd need to be carefully synchronised with the main submit thread. That's
		// not yet implemented since it's not currently worth the hassle, but if someone in the future wants to do it:
		// TODO submit skybox frames in their own thread.
		XrFrameBeginInfo beginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
		if (XrResult res = xrBeginFrame(xr_session.get(), &beginInfo); XR_FAILED(res)) {
			OOVR_LOG_ONCE("SetSkyboxOverride: xrBeginFrame failed");
			if (res == XR_ERROR_CALL_ORDER_INVALID)
				frameAwaitingBegin = false;
			OnFrameCallFailed(res);
			return 1; // VRCompositorError_RequestFailed
		}
		frameAwaitingBegin = false;
		frameBegun = true;
		frameCallFailures = 0;
		// Every way out of here ends the frame
		auto ended = [this] {
			frameBegun = false;
			if (ownFrames)
				ownFrames->OnAppFrameEnd();
		};

		if (skybox_compositor == nullptr)
			skybox_compositor = BaseCompositor::CreateCompositorAPI(pTextures);
		if (skybox_compositor == nullptr) {
			// No compositor for this texture: end the begun frame empty
			XrFrameEndInfo empty{ XR_TYPE_FRAME_END_INFO };
			empty.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
			empty.displayTime = xr_gbl->nextPredictedFrameTime;
			xrEndFrame(xr_session.get(), &empty);
			ended();
			return 1; // VRCompositorError_RequestFailed
		}

		vr::VRTextureBounds_t bounds;
		bounds.uMin = 0.0;
		bounds.uMax = 1.0;
		bounds.vMin = 1.0;
		bounds.vMax = 0.0;

		XrCompositionLayerQuad layerQuad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
		skybox_compositor->Invoke(pTextures, &bounds, layerQuad.subImage);
		if (XR_FAILED(skybox_compositor->LastResult())) {
			// No image this frame: end the begun frame empty
			XrFrameEndInfo empty{ XR_TYPE_FRAME_END_INFO };
			empty.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
			empty.displayTime = xr_gbl->nextPredictedFrameTime;
			xrEndFrame(xr_session.get(), &empty);
			ended();
			return 1; // VRCompositorError_RequestFailed
		}
		layerQuad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
		layerQuad.next = NULL;
		layerQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		layerQuad.space = xr_space_from_ref_space_type(BaseSystem::currentSpace);
		layerQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		layerQuad.pose = { { 0.f, 0.f, 0.f, 1.f },
			{ 0.0f, 0.0f, -0.65f } };
		layerQuad.size = { 1.0f, 1.0f / 1.333f };

		XrCompositionLayerBaseHeader* layers[1];
		layers[0] = (XrCompositionLayerBaseHeader*)&layerQuad;
		XrFrameEndInfo info{ XR_TYPE_FRAME_END_INFO };
		info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		info.displayTime = xr_gbl->nextPredictedFrameTime;
		info.layers = layers;
		info.layerCount = 1;

		OOVR_FAILED_XR_SOFT_ABORT(xrEndFrame(xr_session.get(), &info));
		ended();

	} else {
		OOVR_SOFT_ABORT("Unsupported texture count");
	}

	return 0;
}

void XrBackend::ClearSkyboxOverride()
{
	std::lock_guard<std::mutex> lock(skyboxLock);
	skybox.layerCount = 0;
}

/* Misc compositor */

void XrBackend::RecordFrameTiming(std::chrono::steady_clock::time_point callTime, std::chrono::steady_clock::time_point returnTime,
    XrTime predictedDisplayTime, XrDuration predictedDisplayPeriod)
{
	using ms = std::chrono::duration<float, std::milli>;
	float wait = ms(returnTime - callTime).count();
	if (predictedDisplayPeriod > 0)
		displayPeriodMs = (float)predictedDisplayPeriod / 1e6f;
	if (lastWaitCall != std::chrono::steady_clock::time_point{}) {
		float interval = ms(callTime - lastWaitCall).count();
		float busy = interval - measuredWaitMs; // since the previous xrWaitFrame returned
		measuredDropped = 0;
		// A long pause (loading, the session hidden) is not a frame: skip it rather than drop the resolution for it.
		// Count to this xrWaitFrame's return, since the runtime may be what paused.
		if (ms(returnTime - lastWaitCall).count() < 1000.0f) {
			// Cap each busy sample at a few display periods, so that one hitch (a shader compile, streaming) can't hold
			// the average over budget for dozens of frames. A game that's slow every frame still reads as over budget.
			const float cap = displayPeriodMs > 0.0f ? 3.0f * displayPeriodMs : 1000.0f;
			const float a = measuredIntervalMs == 0.0f ? 1.0f : 0.1f; // ~10-frame smoothing; the app samples one frame
			measuredIntervalMs = interval; // per frame, as SteamVR reports it
			measuredBusyMs += a * (std::min(busy, cap) - measuredBusyMs);

			// Dropped frames are the display slots the runtime skipped since the previous frame. The time between calls
			// would also count the app's frame-to-frame jitter, which the runtime absorbs, as drops.
			if (lastPredictedDisplayTime != 0 && predictedDisplayPeriod > 0 && predictedDisplayTime > lastPredictedDisplayTime) {
				XrDuration slots = (predictedDisplayTime - lastPredictedDisplayTime + predictedDisplayPeriod / 2) / predictedDisplayPeriod;
				measuredDropped = slots > 1 ? (uint32_t)(slots - 1) : 0;
			}
		}
	}
	measuredWaitMs = wait;
	lastWaitCall = callTime;
	lastPredictedDisplayTime = predictedDisplayTime;
}

/**
 * Get frame timing information to be passed to the application
 *
 * Returns true if successful
 */
bool XrBackend::GetFrameTiming(OOVR_Compositor_FrameTiming* pTiming, uint32_t unFramesAgo)
{
	if (!pTiming)
		return false;

	// The struct grew over OpenVR versions, so write no more than the app's m_nSize says it has. A bad m_nSize crashed F1 23
	// here: one bigger than the newest struct is taken as ours, which every game since IVRCompositor_018 has room for.
	// The newest struct is padded to 8 bytes on Windows, which is also what Proton passes to a Linux build.
	constexpr size_t newestSize = (sizeof(vr::Compositor_FrameTiming) + 7) & ~(size_t)7;
	size_t size = pTiming->m_nSize;
	if (size > newestSize) {
		OOVR_LOG_ONCEF("GetFrameTiming: m_nSize %u is bigger than the newest OpenVR struct, using %u", pTiming->m_nSize,
		    (uint32_t)sizeof(OOVR_Compositor_FrameTiming));
		size = sizeof(OOVR_Compositor_FrameTiming);
	}

	// Zero everything except the size field
	if (size > sizeof(pTiming->m_nSize))
		memset(reinterpret_cast<unsigned char*>(pTiming) + sizeof(pTiming->m_nSize), 0, size - sizeof(pTiming->m_nSize));

	if (size >= sizeof(IVRCompositor_018::Compositor_FrameTiming)) {
		pTiming->m_flSystemTimeInSeconds = frameSubmitTimeUs;
		pTiming->m_nFrameIndex = nFrameIndex;

		// A lot of these values we can't get the data for so just use sensible values
		pTiming->m_nNumFramePresents = 1; // number of times this frame was presented
		pTiming->m_nNumMisPresented = 0; // number of times this frame was presented on a vsync other than it was originally predicted to
		pTiming->m_nNumDroppedFrames = 0; // number of additional times previous frame was scanned out
		pTiming->m_nReprojectionFlags = 0;

		// No GPU timers through OpenXR: the app's measured frame time (render + submit, between xrWaitFrame calls) stands
		// in for its GPU time. A GPU-bound app blocks in submit or present, so the proxy grows as the GPU saturates.
		// Before the first measured frame, the old constants.
		if (measuredIntervalMs > 0.0f) {
			pTiming->m_nNumDroppedFrames = measuredDropped;
			pTiming->m_nNumMisPresented = measuredDropped ? 1 : 0;
			pTiming->m_flTotalRenderGpuMs = measuredBusyMs;
			pTiming->m_flPreSubmitGpuMs = measuredBusyMs > 1.0f ? measuredBusyMs - 1.0f : measuredBusyMs;
			pTiming->m_flPostSubmitGpuMs = measuredBusyMs > 1.0f ? 1.0f : 0.0f;
		} else {
			pTiming->m_flPreSubmitGpuMs = 8.0f;
			pTiming->m_flPostSubmitGpuMs = 1.0f;
			pTiming->m_flTotalRenderGpuMs = 9.0f;
		}

		// Use very conservative guesses for these. They are used in F1 22 for dynamic resolution calculations but are not something that is provided
		// through OpenXR. Using conservative values will give a bit more headroom for the game to target realistic frame times.
		pTiming->m_flCompositorRenderGpuMs = 1.5f; // time spend performing distortion correction, rendering chaperone, overlays, etc.
		pTiming->m_flCompositorRenderCpuMs = 3.0f; // time spent on cpu submitting the above work for this frame

		// The time spent in xrWaitFrame is headroom only if the frame made its slot; after a miss it includes skipped slots
		// (and no more than a display period, after a pause)
		const float idle = displayPeriodMs > 0.0f ? std::min(measuredWaitMs, displayPeriodMs.load()) : measuredWaitMs;
		pTiming->m_flCompositorIdleCpuMs = measuredIntervalMs > 0.0f ? (measuredDropped ? 0.0f : idle) : 0.1f;

		/** Miscellaneous measured intervals. */
		pTiming->m_flClientFrameIntervalMs = measuredIntervalMs > 0.0f ? measuredIntervalMs : 11.1f; // time between calls to WaitGetPoses
		pTiming->m_flPresentCallCpuMs = 0.0f; // time blocked on call to present (usually 0.0, but can go long)
		pTiming->m_flWaitForPresentCpuMs = 0.0f; // time spent spin-waiting for frame index to change (not near-zero indicates wait object failure)
		pTiming->m_flSubmitFrameMs = 0.0f; // time spent in IVRCompositor::Submit (not near-zero indicates driver issue)

		/** The following are all relative to this frame's SystemTimeInSeconds */
		pTiming->m_flWaitGetPosesCalledMs = 0.0f;
		pTiming->m_flNewPosesReadyMs = 0.0f;
		pTiming->m_flNewFrameReadyMs = 0.0f; // second call to IVRCompositor::Submit
		pTiming->m_flCompositorUpdateStartMs = 0.0f;
		pTiming->m_flCompositorUpdateEndMs = 0.0f;
		pTiming->m_flCompositorRenderStartMs = 0.0f;

		GetPrimaryHMD()->GetPose(vr::ETrackingUniverseOrigin::TrackingUniverseSeated, &pTiming->m_HmdPose, ETrackingStateType::TrackingStateType_Rendering);

		return true;
	}

	return false;
}

/* D3D Mirror textures */
/* #if defined(SUPPORT_DX) */
void XrBackend::KeepForMirror(vr::EVREye eye, const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds)
{
#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	std::lock_guard<std::mutex> lock(mirrorLock);
	auto** mirror = (ID3D11Texture2D**)&mirrorTexture[eye];
	if (!texture) {
		if (*mirror)
			(*mirror)->Release();
		*mirror = nullptr;
		return;
	}
	// Nothing is copied until the game asks for a mirror texture. A Direct3D 10 texture (or any other) has none.
	if (!mirrorWanted || texture->eType != vr::TextureType_DirectX || !texture->handle
#if defined(SUPPORT_DX10)
	    || DX10Compositor::AsD3D10Texture(texture)
#endif
	)
		return;
	DX11Compositor::MirrorUpdate(texture, bounds, mirror);
#endif
}

IBackend::openvr_enum_t XrBackend::GetMirrorTextureD3D11(vr::EVREye eEye, void* pD3D11DeviceOrResource, void** ppD3D11ShaderResourceView)
{
#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	if (eEye != vr::Eye_Left && eEye != vr::Eye_Right)
		return 1; // VRCompositorError_RequestFailed
	// A view of the eye's mirror texture, which every frame from now on is copied into. Asked for before the game's
	// first frame, the texture has the size the game is told to render at.
	uint32_t width = 0, height = 0;
	hmd->GetRecommendedRenderTargetSize(&width, &height);
	std::lock_guard<std::mutex> lock(mirrorLock);
	mirrorWanted = true;
	int result = DX11Compositor::MirrorView((ID3D11Texture2D**)&mirrorTexture[eEye], (IUnknown*)pD3D11DeviceOrResource, width, height,
	    (ID3D11ShaderResourceView**)ppD3D11ShaderResourceView);
	if (result == 0)
		mirrorViews.insert(*ppD3D11ShaderResourceView);
	return result;
#else
	return 106; // VRCompositorError_SharedTexturesNotSupported
#endif
}
void XrBackend::ReleaseMirrorTextureD3D11(void* pD3D11ShaderResourceView)
{
#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	// Only views handed out here: anything else isn't ours to release
	std::lock_guard<std::mutex> lock(mirrorLock);
	if (pD3D11ShaderResourceView && mirrorViews.erase(pD3D11ShaderResourceView))
		((IUnknown*)pD3D11ShaderResourceView)->Release();
#endif
}
/* #endif */
/** Returns the points of the Play Area. */
bool XrBackend::GetPlayAreaPoints(vr::HmdVector3_t* points, int* count)
{
	if (count)
		*count = 0;
	if (!xr_session.get())
		return false; // the session is lost

	XrExtent2Df bounds;
	XrResult res = xrGetReferenceSpaceBoundsRect(xr_session.get(), XR_REFERENCE_SPACE_TYPE_STAGE, &bounds);

	if (res == XR_SPACE_BOUNDS_UNAVAILABLE || XR_FAILED(res))
		return false; // no play area (set up, or at all)

	if (count)
		*count = 4;

	// The origin of the free space is centred around the player
	// TODO if we're using the Oculus runtime, grab it's native handle and get the full polygon
	if (points) {
		points[0] = vr::HmdVector3_t{ -bounds.width / 2, 0, -bounds.height / 2 };
		points[1] = vr::HmdVector3_t{ bounds.width / 2, 0, -bounds.height / 2 };
		points[2] = vr::HmdVector3_t{ bounds.width / 2, 0, bounds.height / 2 };
		points[3] = vr::HmdVector3_t{ -bounds.width / 2, 0, bounds.height / 2 };
	}

	return true;
}
/** Determine whether the bounds are showing right now **/
bool XrBackend::AreBoundsVisible()
{
	// OpenXR doesn't say whether the runtime is showing its boundary
	return false;
}
/** Set the boundaries to be visible or not (although setting this to false shouldn't affect
 * what happens if the player moves their hands too close and shows it that way) **/
void XrBackend::ForceBoundsVisible(bool status)
{
	// The runtime shows its boundary itself; OpenXR has no call to force it
}

bool XrBackend::IsInputAvailable()
{
	return sessionState == XR_SESSION_STATE_FOCUSED;
}

void XrBackend::PumpEvents()
{
	// Poll for OpenXR events
	// TODO filter by session?
	while (true) {
		XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
		XrResult res = xr_instance ? xrPollEvent(xr_instance, &ev) : XR_ERROR_HANDLE_INVALID;
		if (XR_FAILED(res)) {
			OOVR_LOG_ONCE("xrPollEvent failed: no events this frame");
			if (res == XR_ERROR_INSTANCE_LOST)
				OnSessionLost();
			break;
		}

		if (res == XR_EVENT_UNAVAILABLE) {
			break;
		}

		if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
			auto* changed = (XrEventDataSessionStateChanged*)&ev;
			// A state change for a session already destroyed (around a restart) is old news
			if (changed->session != xr_session.get())
				continue;
			sessionState = changed->state;

			// Monado bug: it returns 0 for this value (at least for the first two states)
			// Make sure this is actually greater than 0, otherwise this will mess up xr_gbl->GetBestTime()
			if (changed->time > 0 && xr_gbl)
				xr_gbl->latestTime = changed->time;

			OOVR_LOGF("Switch to OpenXR state %d", sessionState);

			switch (sessionState) {
			case XR_SESSION_STATE_READY: {
				OOVR_LOG("Hit ready state, begin session...");
				// Start the session running - this means we're supposed to start submitting frames
				XrSessionBeginInfo beginInfo{ XR_TYPE_SESSION_BEGIN_INFO };
				beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				// Not while a frame of either side's is in flight (see frameOwner)
				std::lock_guard<std::mutex> owner(frameOwner);
				if (XrResult begin = xrBeginSession(xr_session.get(), &beginInfo); XR_FAILED(begin)) {
					// READY doesn't come again: without a running session the game is told to quit
					OOVR_LOGF("xrBeginSession failed: %d", (int)begin);
					OnSessionLost();
					break;
				}
				sessionActive = true;
				frameAwaitingBegin = false;
				frameBegun = false;
				frameCallFailures = 0;
				break;
			}
			case XR_SESSION_STATE_STOPPING: {
				// End the session. The session is still valid and we can still query some information
				// from it, but we're not allowed to submit frames anymore. This is done when the engagement
				// sensor detects the user has taken off the headset, for example.
				// Not while a frame of either side's is in flight (see frameOwner)
				std::lock_guard<std::mutex> owner(frameOwner);
				if (sessionActive && XR_FAILED(xrEndSession(xr_session.get()))) // could be inactive if we missed XR_SESSION_STATE_READY
					OOVR_LOG("xrEndSession failed");
				sessionActive = false;
				renderingFrame = false;
				frameBegun = false;
				frameAwaitingBegin = false;
				break;
			}
			case XR_SESSION_STATE_EXITING: {
				OOVR_LOGF("Exiting");
				// The runtime ending the session (the user quitting from its menu, say) is the game's cue to quit;
				// OpenComposite ending it itself isn't
				if (!exitExpected)
					OnSessionLost();
				break;
			}
			case XR_SESSION_STATE_LOSS_PENDING: {
				// If the headset is unplugged or the user decides to exit the app
				// TODO just kill the app after awhile, unless it sends a message to stop that - read the OpenVR wiki docs for more info
				OnSessionLost();
				break;
			}
			default:
				// suppress clion warning about missing branches
				break;
			}
		} else if (ev.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
			UpdateInteractionProfile();
		} else if (ev.type == OC_XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB) {
			// Prop_DisplayFrequency_Float asks the runtime again
			auto* changed = (const OcXrEventDataDisplayRefreshRateChangedFB*)&ev;
			OOVR_LOGF("The display's refresh rate changed from %.2f to %.2f Hz", changed->fromDisplayRefreshRate, changed->toDisplayRefreshRate);
			BaseSystem* system = GetUnsafeBaseSystem();
			if (system) {
				VREvent_t event = { .eventType = VREvent_PropertyChanged, .trackedDeviceIndex = vr::k_unTrackedDeviceIndex_Hmd };
				event.data.property.container = vr::k_ulInvalidPropertyContainer;
				event.data.property.prop = vr::Prop_DisplayFrequency_Float;
				system->_EnqueueEvent(event);
			}
		}

	} // while loop

	/*
	   We check for AreActionsLoaded here because:
	   1. Games using legacy input call xrSyncActions every frame anyway, so the runtime should
	      give us an interaction profile without us forcing it
	   2. Games using an action manifest should be calling UpdateActionState every frame, which calls xrSyncActions.
	      This means the only games we wouldn't be able to confidently grab an interaction profile from
	      would be ones where an action manifest is loaded but UpdateActionState is not being called
	      (because the game checks IsTrackedDeviceConnected or something),
	      and hopefully no game like that exists.
	   Some runtimes (WMR) do not instantly return an interaction profile,
	   so we will keep tryinig to query it until it does.

	   Note that we check that the session is focused because this means that the application
	   has already submitted a frame, that frame is visible, and we have input focus.
	   Waiting until the application has input focus allows us to avoid unnecessarily restarting the
	   session when we can't even receive input anyway, as well as before the session is restarted for
	   the temporary session.
   */
	// Not while a session is being ended (ShutdownSession pumps events while it waits, maybe under the input restart)
	BaseInput* input = GetUnsafeBaseInput();
	if (input && !exitExpected && !input->AreActionsLoaded() && sessionState == XR_SESSION_STATE_FOCUSED && !Hand(XrController::XCT_LEFT) && !Hand(XrController::XCT_RIGHT)) {
		QueryForInteractionProfile();
	}
}

void XrBackend::OnFrameCallFailed(XrResult res)
{
	renderingFrame = false;
	if (res == XR_ERROR_SESSION_LOST || res == XR_ERROR_INSTANCE_LOST || ++frameCallFailures >= 5)
		OnSessionLost();
	// No waited frame to pace the game: about one frame's wait instead of a busy loop
	std::this_thread::sleep_for(std::chrono::milliseconds(11));
}

void XrBackend::OnSessionLost()
{
	sessionActive = false;
	renderingFrame = false;
	if (sessionLost)
		return;
	sessionLost = true;
	OOVR_LOG("The OpenXR session is lost: telling the game to quit");
	EnqueueQuit();
}

// Own frames call xrEndFrame on their own thread, where the runtime may use the session's device while the game's
// threads use it too. A D3D11 device's immediate context is made safe for that (multithread protection, logged once);
// a D3D12 queue is free-threaded. A Vulkan queue or an OpenGL context can't be shared with the game's threads that
// way: false, logged once, and there are no own frames.
static bool DeviceReadyForOwnFrames(const void* binding)
{
	const auto* header = (const XrBaseInStructure*)binding;
	if (!header)
		return false;
	switch (header->type) {
#if defined(_WIN32) && defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	case XR_TYPE_GRAPHICS_BINDING_D3D11_KHR: {
		ID3D11Device* device = ((const XrGraphicsBindingD3D11KHR*)binding)->device;
		ComPtr<ID3D11DeviceContext> context;
		if (device)
			device->GetImmediateContext(&context);
		ComPtr<ID3D11Multithread> multithread;
		if (!context || FAILED(context.As(&multithread)) || !multithread) {
			OOVR_LOG_ONCE("ownFrames: the D3D11 device has no ID3D11Multithread, so there are no own frames");
			return false;
		}
		if (!multithread->GetMultithreadProtected()) {
			multithread->SetMultithreadProtected(TRUE);
			OOVR_LOG_ONCE("ownFrames: the session's D3D11 immediate context is now multithread protected");
		}
		return true;
	}
#endif
#if defined(_WIN32) && defined(SUPPORT_DX) && defined(SUPPORT_DX12)
	case XR_TYPE_GRAPHICS_BINDING_D3D12_KHR:
		return true;
#endif
	default:
		OOVR_LOG_ONCEF("ownFrames: no own frames on a session of graphics binding type %d (a Vulkan queue or an OpenGL context "
		               "is the game's own)",
		    (int)header->type);
		return false;
	}
}

void XrBackend::OnSessionCreated()
{
	sessionLost = false;
	exitExpected = false;
	// The gap from the old session's last frame isn't dropped frames
	RestartFrameTiming();

	sessionState = XR_SESSION_STATE_UNKNOWN;
	sessionActive = false;
	renderingFrame = false;
	frameAwaitingBegin = false;
	frameBegun = false;
	frameCallFailures = 0;

	PumpEvents();

	// Wait until we transition to the idle state.
	// This sets the time, so OpenXR calls which use that will work correctly.
	int count = 0;
	while (sessionState == XR_SESSION_STATE_UNKNOWN && !sessionLost && count++ < 10) {
		const int durationMs = 250;

		OOVR_LOGF("No session transition yet received, waiting %dms ...", durationMs);

#ifdef _WIN32
		Sleep(durationMs);
#else
		struct timespec ts = { 0, durationMs * 1000000 };
		nanosleep(&ts, &ts);
#endif

		PumpEvents();
	}
	if (sessionState == XR_SESSION_STATE_UNKNOWN && !sessionLost)
		OOVR_LOG("No session transition received yet: carrying on, later PumpEvents calls take the session's states");
	if (sessionLost) {
		// OnSessionLost has told the game to quit
		return;
	}

	// A scene application's session makes it the scene application for every program
	if (BaseClientCore::appType == vr::VRApplication_Scene)
		BaseApplications::SetSceneProcess(true);

	// Frames of OpenLamina's own while the game runs none and there's a skybox or an overlay to show
	StopOwnFrames();
	if (ownFramesStopped || !oovr_global_configuration.OwnFrames() || !DeviceReadyForOwnFrames(GetCurrentGraphicsBinding()))
		return;
	ownFrames = std::make_unique<OwnFrameLoop>(
	    OwnFrameLoop::Mode::WhenAppIdle,
	    [this](XrTime, std::vector<const XrCompositionLayerBaseHeader*>& layers) {
		    // RunOwnFrame holds frameOwner, under which the skybox and the game's last frame are replaced: the skybox in
		    // the game's place, else its last frame again (which the runtime reprojects); then the overlays
		    {
			    std::lock_guard<std::mutex> lock(skyboxLock);
			    for (int i = 0; i < skybox.layerCount; i++)
				    layers.push_back((const XrCompositionLayerBaseHeader*)&skybox.layers[i]);
		    }
		    if (layers.empty() && haveLastProjection)
			    layers.push_back((const XrCompositionLayerBaseHeader*)&lastProjection);
		    BaseOverlay* overlay = GetUnsafeBaseOverlay();
		    if (!overlay)
			    return;
		    // RunOwnFrame holds the overlays' lock until the frame has ended
		    XrCompositionLayerBaseHeader const* const* headers = nullptr;
		    int count = overlay->_BuildLayers(nullptr, headers, false);
		    layers.insert(layers.end(), headers, headers + count);
	    },
	    std::chrono::milliseconds(100),
	    [this](const OwnFrameLoop::LayerSource& source, const std::function<bool()>& wanted) {
		    return RunOwnFrame(source, wanted);
	    },
	    [this] {
		    BaseOverlay* overlay = GetUnsafeBaseOverlay();
		    return SkyboxShown() || (overlay && overlay->_AnyVisible());
	    });
}

// Here rather than in OwnFrameLoop.cpp, which then needs nothing of the backend's
OwnFrameLoop::OwnFrameLoop(Mode mode, LayerSource source, std::chrono::milliseconds idleAfter)
    : OwnFrameLoop(mode, std::move(source), idleAfter, [](const LayerSource& source, const std::function<bool()>& wanted) -> XrDuration {
	    BackendManager* manager = BackendManager::InstancePtr();
	    auto* backend = manager ? (XrBackend*)manager->GetBackendInstance() : nullptr;
	    return backend ? backend->RunOwnFrame(source, wanted) : 0;
    })
{
}

bool XrBackend::RunOwnFramesWhile(const std::function<bool()>& keepGoing)
{
	if (!ownFrames)
		return false;
	ownFrames->RunWhile(keepGoing);
	return true;
}

void XrBackend::EndOpenAppFrame()
{
	// As if the game had submitted, keeping whether it ends its frames from PostPresentHandoff
	SubmitFrames(false, postPresentStatus);
}

void XrBackend::StopOwnFrames(bool forGood)
{
	if (forGood)
		ownFramesStopped = true;
	// Joins its thread, which doesn't wait for the session lock that a session restart here holds
	ownFrames.reset();
}

XrDuration XrBackend::RunOwnFrame(const OwnFrameLoop::LayerSource& source, const std::function<bool()>& wanted)
{
	// The session lock (shared) first, as the game's threads take it, but never waited for: a session restart holds
	// it exclusively while it stops the own frames. Tried until it's free or the frame isn't wanted any more.
	std::optional<SessionLock> lock;
	while (true) {
		if (!wanted())
			return 0;
		lock.emplace(xr_session, SessionLock::TryShared{});
		if (lock->IsHeld())
			break;
		lock.reset();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	std::unique_lock<std::mutex> owner(frameOwner);
	// The game's frame is open (it ends it), or the session isn't running: no frame
	if (!wanted() || !sessionActive || frameBegun || !xr_session.get() || !xr_gbl)
		return 0;

	XrFrameState state{ XR_TYPE_FRAME_STATE };
	if (frameAwaitingBegin) {
		// A frame the game waited for and couldn't begin: this one begins it instead
		state.predictedDisplayTime = xr_gbl->nextPredictedFrameTime;
		state.predictedDisplayPeriod = (XrDuration)(displayPeriodMs * 1e6f);
		state.shouldRender = XR_TRUE;
	} else {
		XrFrameWaitInfo waitInfo{ XR_TYPE_FRAME_WAIT_INFO };
		if (XrResult res = xrWaitFrame(xr_session.get(), &waitInfo, &state); XR_FAILED(res)) {
			OOVR_LOG_ONCE("xrWaitFrame failed for an own frame: skipping it");
			OnFrameCallFailed(res);
			return 0;
		}
		if (state.predictedDisplayPeriod > 0)
			displayPeriodMs = (float)state.predictedDisplayPeriod / 1e6f;
		xr_gbl->nextPredictedFrameTime = state.predictedDisplayTime;
		frameAwaitingBegin = true;
	}

	XrFrameBeginInfo beginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
	if (XrResult res = xrBeginFrame(xr_session.get(), &beginInfo); XR_FAILED(res)) {
		OOVR_LOG_ONCE("xrBeginFrame failed for an own frame: dropping it");
		if (res == XR_ERROR_CALL_ORDER_INVALID)
			frameAwaitingBegin = false;
		OnFrameCallFailed(res);
		return 0;
	}
	frameAwaitingBegin = false;
	frameCallFailures = 0;
	// Not one of the game's frames, so the time since its last one isn't a frame interval
	RestartFrameTiming();
	FrameStats::Get().OnFrameStart();

	XrFrameEndInfo info{ XR_TYPE_FRAME_END_INFO };
	info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	info.displayTime = state.predictedDisplayTime;

	// The source's layers (the overlays' among them) stay as they are until the frame has ended
	BaseOverlay* overlay = GetUnsafeBaseOverlay();
	std::unique_lock<std::recursive_mutex> overlayLock;
	if (overlay)
		overlayLock = std::unique_lock(overlay->_LayersLock());
	std::vector<const XrCompositionLayerBaseHeader*> layers;
	if (state.shouldRender && source)
		source(state.predictedDisplayTime, layers);
	info.layers = layers.data();
	info.layerCount = (uint32_t)layers.size();
	XrResult res = xrEndFrame(xr_session.get(), &info);
	if (XR_FAILED(res) && !layers.empty()) {
		// Ended without them, so the frame doesn't stay open
		OOVR_LOG_ONCEF("xrEndFrame failed for an own frame with %u layers (%d): ending it without them", info.layerCount, (int)res);
		info.layers = nullptr;
		info.layerCount = 0;
		res = xrEndFrame(xr_session.get(), &info);
	}
	if (XR_FAILED(res))
		OOVR_LOG_ONCEF("xrEndFrame failed for an own frame: %d", (int)res);
	else
		FrameStats::Get().OnOwnFrame();

	XrDuration period = state.predictedDisplayPeriod > 0 ? state.predictedDisplayPeriod : (XrDuration)(displayPeriodMs * 1e6f);
	return period > 0 ? period : (XrDuration)11000000;
}

void XrBackend::PrepareForSessionShutdown()
{
	StopOwnFrames();
	for (std::unique_ptr<Compositor>& c : compositors) {
		c.reset();
	}
	skybox_compositor.reset();
	{
		std::lock_guard<std::mutex> lock(skyboxLock);
		skybox = Skybox();
	}
	skyboxReader.reset();
	haveLastProjection = false;
	for (std::unique_ptr<Compositor>& c : depthCompositors)
		c.reset();
	for (XrCompositionLayerProjectionView& view : projectionViews)
		view.next = nullptr;
	overlay_compositors.clear();
	// Swapchains filled from memory let go of theirs now, while the session and its device are still there
	CpuSwapchain::ReleaseAll();
	if (infoSet != XR_NULL_HANDLE) {
		OC_XR_LOG(xrDestroyActionSet(infoSet));
		infoSet = XR_NULL_HANDLE;
		infoAction = XR_NULL_HANDLE;
	}
}

// On Android, add an event poll function for use while sleeping
#ifdef ANDROID
void OpenComposite_Android_EventPoll()
{
	BackendManager::Instance().PumpEvents();
}
#endif

bool XrBackend::IsGraphicsConfigured()
{
	return usingApplicationGraphicsAPI;
}

void XrBackend::OnOverlayTexture(const vr::Texture_t* texture)
{
	if (!usingApplicationGraphicsAPI)
		CheckOrInitCompositors(texture);
}

void XrBackend::RegisterOverlayCompositor(std::shared_ptr<Compositor> compositor)
{
	overlay_compositors.push_back(compositor);
}

void XrBackend::UnregisterOverlayCompositor(std::shared_ptr<Compositor> compositor)
{
	std::erase_if(overlay_compositors, [c = std::move(compositor)](std::shared_ptr<Compositor>& comp) {
		return c == comp;
	});
}

void XrBackend::UpdateInteractionProfile()
{
	if (sessionLost || !xr_session.get())
		return; // the controllers stay as they were
	struct hand_info {
		const char* pathstr;
		std::shared_ptr<XrController>& controller;
		const XrController::XrControllerType hand;
	};

	hand_info hands[] = {
		{ .pathstr = "/user/hand/left", .controller = hand_left, .hand = XrController::XCT_LEFT },
		{ .pathstr = "/user/hand/right", .controller = hand_right, .hand = XrController::XCT_RIGHT }
	};

	for (hand_info& info : hands) {
		XrInteractionProfileState state{ XR_TYPE_INTERACTION_PROFILE_STATE };
		XrPath path = XR_NULL_PATH;
		OC_XR_LOG(xrStringToPath(xr_instance, info.pathstr, &path));
		// On failure state.interactionProfile stays XR_NULL_PATH: no controller for this hand yet
		OC_XR_LOG(xrGetCurrentInteractionProfile(xr_session.get(), path, &state));

		// interaction profile detected
		if (state.interactionProfile != XR_NULL_PATH) {
			uint32_t tmp = 0;
			char path_name[XR_MAX_PATH_LENGTH] = {};
			OC_XR_LOG(xrPathToString(xr_instance, state.interactionProfile, XR_MAX_PATH_LENGTH, &tmp, path_name));

			for (const std::unique_ptr<InteractionProfile>& profile : InteractionProfile::GetProfileList()) {
				if (profile->GetPath() == path_name) {
					OOVR_LOGF("%s - Using interaction profile: %s", info.pathstr, path_name);
					{
						std::unique_lock handsLock(hands_mutex);
						info.controller = std::make_shared<XrController>(info.hand, *profile);
					}
					hmd->SetInteractionProfile(profile.get());
					BaseSystem* system = GetUnsafeBaseSystem();
					if (system) {
						VREvent_t event = {
							.eventType = VREvent_TrackedDeviceActivated,
							.trackedDeviceIndex = (TrackedDeviceIndex_t)info.hand + 1
						};
						system->_EnqueueEvent(event);
						event = {
							.eventType = VREvent_TrackedDeviceUpdated,
							.trackedDeviceIndex = 0
						};
						system->_EnqueueEvent(event);
					}
					break;
				}
			}
			if (!Hand(XrController::XCT_LEFT) && !Hand(XrController::XCT_RIGHT)) {
				// Runtime returned an unknown interaction profile: no controller until it gives one we know
				OOVR_LOGF("Runtime unexpectedly returned an unknown interaction profile: %s", path_name);
			}
		} else {
			// interaction profile lost/not detected
			OOVR_LOGF("%s - No interaction profile detected", info.pathstr);
			if (Hand(info.hand)) {
				{
					std::unique_lock handsLock(hands_mutex);
					info.controller.reset();
				}
				BaseSystem* system = GetUnsafeBaseSystem();
				if (system) {
					VREvent_t event = {
						.eventType = VREvent_TrackedDeviceDeactivated,
						.trackedDeviceIndex = (TrackedDeviceIndex_t)info.hand + 1
					};
					system->_EnqueueEvent(event);
				}
			}
		}
	}

	CreateGenericTrackers();
}

void XrBackend::CreateGenericTrackers()
{
	if (!xr_ext->xrMndxXdevSpace_Available())
		return;

	std::unique_lock lock(generic_trackers_mutex);

	OOVR_LOGF("Checking for generic trackers...");

	// should get cleared every time this function is run to avoid stale trackers
	generic_trackers.clear();

	// list containing all generic tracked devices
	XrXDevListMNDX generic_tracker_list;
	std::vector<XrXDevIdMNDX> generic_tracker_ids(MAX_GENERIC_TRACKERS);
	InteractionProfile* profile = InteractionProfile::GetProfileByPath("/interaction_profiles/htc/vive_tracker_htcx");
	if (!profile)
		return;

	uint32_t xdev_count = 0;

	XrCreateXDevListInfoMNDX create_info = {
		.type = XR_TYPE_CREATE_XDEV_LIST_INFO_MNDX
	};

	XrXDevPropertiesMNDX cur_properties = {
		.type = XR_TYPE_XDEV_PROPERTIES_MNDX,
	};

	XrGetXDevInfoMNDX cur_info = {
		.type = XR_TYPE_GET_XDEV_INFO_MNDX,
	};

	// No generic trackers if the runtime can't list them
	if (XR_FAILED(xr_ext->xrCreateXDevListMNDX(xr_session.get(), &create_info, &generic_tracker_list))) {
		OOVR_LOG("xrCreateXDevListMNDX failed: no generic trackers");
		generic_tracker_list = XR_NULL_HANDLE;
		return;
	}
	if (XR_FAILED(xr_ext->xrEnumerateXDevsMNDX(generic_tracker_list, MAX_GENERIC_TRACKERS, &xdev_count, generic_tracker_ids.data()))) {
		OOVR_LOG("xrEnumerateXDevsMNDX failed: no generic trackers");
		xr_ext->xrDestroyXDevListMNDX(generic_tracker_list);
		return;
	}

	std::vector<std::string> forced_tracker_serials{};
	{
		auto forced_tracker_serials_str = GetEnv("OPENLAMINA_TRACKER_SERIALS");
		if (forced_tracker_serials_str.empty())
			forced_tracker_serials_str = GetEnv("OPENCOMPOSITE_TRACKER_SERIALS"); // its name before OpenLamina
		if (!forced_tracker_serials_str.empty()) {
			size_t separator_pos{ std::string::npos };
			while ((separator_pos = forced_tracker_serials_str.find(';')) != std::string::npos) {
				auto serial = forced_tracker_serials_str.substr(0, separator_pos);
				forced_tracker_serials.push_back(serial);
				forced_tracker_serials_str.erase(0, separator_pos + 1);
			}
			if (!forced_tracker_serials_str.empty()) {
				forced_tracker_serials.push_back(forced_tracker_serials_str);
			}
		}
	}

	// filter out non-tracker devices
	std::erase_if(generic_tracker_ids, [&](XrXDevIdMNDX id) {
		if (xdev_count == 0) {
			return true;
		}
		cur_info.id = id;

		OC_XR_LOG(xr_ext->xrGetXDevPropertiesMNDX(generic_tracker_list, &cur_info, &cur_properties));

		xdev_count--;

		std::string name = cur_properties.name;
		std::string serial = cur_properties.serial;

		if (!cur_properties.canCreateSpace)
			return true;

		OOVR_LOGF("Found usable xdev '%s', serial '%s'", name.c_str(), serial.c_str());

		if (forced_tracker_serials.size() > 0 && std::find(forced_tracker_serials.cbegin(), forced_tracker_serials.cend(), serial) != forced_tracker_serials.cend()) {
			return false;
		}
		return name.find("Tracker") == std::string::npos;
	});

	OOVR_LOGF("Found %zu generic trackers", generic_tracker_ids.size());

	OOVR_LOG("Creating generic trackers...");
	for (const XrXDevIdMNDX id : generic_tracker_ids) {
		int index = generic_trackers.size();

		if (index >= MAX_GENERIC_TRACKERS) {
			OOVR_LOGF("More trackers present than allowed (%d), skipping..", MAX_GENERIC_TRACKERS);
			break;
		}

		cur_info.id = id;

		OC_XR_LOG(xr_ext->xrGetXDevPropertiesMNDX(generic_tracker_list, &cur_info, &cur_properties));

		std::string serial = cur_properties.serial;
		std::string name = cur_properties.name;

		OOVR_LOGF("Generic Tracker: %s, Serial: %s, Index: %d, ID: %" PRIu64, cur_properties.name, cur_properties.serial, index, id);

		// create space
		XrSpace space = XR_NULL_HANDLE;
		XrPosef pose = { .orientation = { 0, 0, 0, 1 }, .position = { 0, 0, 0 } };
		XrCreateXDevSpaceInfoMNDX create_space_info = {
			.type = XR_TYPE_CREATE_XDEV_SPACE_INFO_MNDX,
			.next = NULL,
			.xdevList = generic_tracker_list,
			.id = id,
			.offset = pose,
		};

		if (XR_FAILED(xr_ext->xrCreateXDevSpaceMNDX(xr_session.get(), &create_space_info, &space))) {
			OOVR_LOGF("xrCreateXDevSpaceMNDX failed: no tracker for serial %s", cur_properties.serial);
			continue;
		}

		generic_trackers.push_back(std::make_unique<XrGenericTracker>(*profile, cur_properties, index, space));

		BaseSystem* system = GetUnsafeBaseSystem();
		if (system) {
			VREvent_t event = {
				.eventType = VREvent_TrackedDeviceActivated,
				.trackedDeviceIndex = (TrackedDeviceIndex_t)generic_trackers.at(index)->DeviceIndex()
			};
			system->_EnqueueEvent(event);
		}
	}

	// cleanup
	xr_ext->xrDestroyXDevListMNDX(generic_tracker_list);
}

void XrBackend::MaybeRestartForInputs()
{
	// if we haven't attached any actions to the session (infoSet or game actions), no need to restart
	// The session lock before inputRestartMutex, the order QueryForInteractionProfile has them in under Submit
	auto sessionLock = xr_session.lock();
	const std::lock_guard<std::mutex> lock(inputRestartMutex);
	BaseInput* input = GetUnsafeBaseInput();
	if (infoSet == XR_NULL_HANDLE && (!input || !input->AreActionsLoaded()))
		return;

	OOVR_LOG("Restarting session for inputs...");
	if (XR_FAILED(DrvOpenXR::SetupSession())) {
		// No session to put the actions on: the game is told to quit
		auto* backend = (XrBackend*)BackendManager::Instance().GetBackendInstance();
		if (backend)
			backend->OnSessionLost();
		return;
	}
	OOVR_LOG("Session restart successful!");
}

void XrBackend::QueryForInteractionProfile()
{
	// Note that we want to avoid using BaseInput here because it would allow for games to call GetControllerState before rendering
	// and then we'd have to recreate the session twice, once for the input state and once for when the game submits a frame
	const std::lock_guard<std::mutex> lock(inputRestartMutex);
	if (subactionPaths[0] == XR_NULL_PATH) {
		OC_XR_LOG(xrStringToPath(xr_instance, "/user/hand/left", &subactionPaths[0]));
		OC_XR_LOG(xrStringToPath(xr_instance, "/user/hand/right", &subactionPaths[1]));
	}

	if (infoSet == XR_NULL_HANDLE) {
		OOVR_LOG("Creating infoset");
		CreateInfoSet();
		BindInfoSet();
	}

	// Interaction profiles are updated after xrSyncActions, so we'll try to make the runtime give us one by calling xrSyncActions.
	XrActiveActionSet active[2] = {
		{ .actionSet = infoSet, .subactionPath = subactionPaths[0] },
		{ .actionSet = infoSet, .subactionPath = subactionPaths[1] },
	};

	XrActionsSyncInfo info{ XR_TYPE_ACTIONS_SYNC_INFO };
	info.countActiveActionSets = 2;
	info.activeActionSets = active;

	OC_XR_LOG(xrSyncActions(xr_session.get(), &info));
}

void XrBackend::CreateInfoSet()
{
	XrActionSetCreateInfo set_info{ XR_TYPE_ACTION_SET_CREATE_INFO };
	strcpy_s(set_info.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE, "openlamina-internal-info-set");
	strcpy_s(set_info.localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE, "OpenLamina internal info set");
	OC_XR_LOG(xrCreateActionSet(xr_instance, &set_info, &infoSet));

	XrActionCreateInfo act_info{ XR_TYPE_ACTION_CREATE_INFO };
	strcpy_s(act_info.actionName, XR_MAX_ACTION_NAME_SIZE, "openlamina-internal-info-act");
	strcpy_s(act_info.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, "OpenLamina internal info action");
	act_info.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
	act_info.countSubactionPaths = std::size(subactionPaths);
	act_info.subactionPaths = subactionPaths;
	OC_XR_LOG(xrCreateAction(infoSet, &act_info, &infoAction));
}

void XrBackend::BindInfoSet()
{
	for (const std::unique_ptr<InteractionProfile>& profile : InteractionProfile::GetProfileList()) {
		XrPath interactionProfilePath = XR_NULL_PATH;
		OC_XR_LOG(xrStringToPath(xr_instance, profile->GetPath().c_str(), &interactionProfilePath));

		if (!profile->CanHaveBindings())
			continue;

		XrInteractionProfileSuggestedBinding suggestedBindings{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };

		std::vector<XrActionSuggestedBinding> bindings;
		using namespace std::string_literals;
		// grabs the first found paths ending in /click for each subaction path
		for (const std::string& path_name : { "/user/hand/left"s, "/user/hand/right"s }) {
			auto click_path = std::ranges::find_if(profile->GetValidInputPaths(),
			    [&path_name](std::string s) -> bool {
				    return s.find("/click") != s.npos && s.find(path_name) != s.npos;
			    });
			XrPath path = XR_NULL_PATH;
			OC_XR_LOG(xrStringToPath(xr_instance, click_path->c_str(), &path));
			if (infoAction != XR_NULL_HANDLE && path != XR_NULL_PATH) // one bad entry would lose the profile's suggestion
				bindings.push_back({ .action = infoAction, .binding = path });

			suggestedBindings.interactionProfile = interactionProfilePath;
			suggestedBindings.suggestedBindings = bindings.data();
			suggestedBindings.countSuggestedBindings = bindings.size();

			OC_XR_LOG(xr_suggest_bindings_logged(suggestedBindings));
		}
	}

	// Attach the info set by itself. We will have to restart the session once the game attaches its real inputs.
	XrSessionActionSetsAttachInfo info{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	info.countActionSets = 1;
	info.actionSets = &infoSet;
	OC_XR_LOG(xrAttachSessionActionSets(xr_session.get(), &info));
}

const void* XrBackend::GetCurrentGraphicsBinding()
{
	if (graphicsBinding) {
		return graphicsBinding->asVoid();
	}
	// nullptr (xrCreateSession then fails, which its caller handles) if there's no graphics API at all
	if (!temporaryGraphics)
		return nullptr;
	return temporaryGraphics->GetGraphicsBinding();
}

#ifdef SUPPORT_VK
void XrBackend::VkGetPhysicalDevice(VkInstance instance, VkPhysicalDevice* out)
{
	*out = VK_NULL_HANDLE;
	if (instance == VK_NULL_HANDLE) {
		OOVR_LOG("VkGetPhysicalDevice: no app instance");
		return;
	}

	// The runtime's own answer for the game's instance, whatever the temporary device's API is
	if (xr_ext && xr_ext->xrGetVulkanGraphicsDeviceKHR_Available()) {
		// Asked first, as the extension wants (its answer isn't needed)
		XrGraphicsRequirementsVulkanKHR requirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR };
		OC_XR_LOG(xr_ext->xrGetVulkanGraphicsRequirementsKHR(xr_instance, xr_system, &requirements));
		VkPhysicalDevice device = VK_NULL_HANDLE;
		if (XR_SUCCEEDED(xr_ext->xrGetVulkanGraphicsDeviceKHR(xr_instance, xr_system, instance, &device)) && device) {
			*out = device;
			return;
		}
		OOVR_LOG_ONCE("VkGetPhysicalDevice: xrGetVulkanGraphicsDeviceKHR failed for the game's instance");
	}

	// Without Vulkan in the runtime: the physical device on the runtime's D3D adapter (frames then go through D3D11)
	uint64_t luid = 0;
	if (BaseSystem::_RuntimeAdapterLuid(&luid)) {
		uint32_t count = 0;
		if (vkEnumeratePhysicalDevices(instance, &count, nullptr) == VK_SUCCESS) {
			std::vector<VkPhysicalDevice> devices(count);
			if (vkEnumeratePhysicalDevices(instance, &count, devices.data()) >= 0) {
				devices.resize(count);
				for (VkPhysicalDevice phy : devices) {
					VkPhysicalDeviceIDProperties id = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
					VkPhysicalDeviceProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id };
					vkGetPhysicalDeviceProperties2(phy, &props);
					uint64_t deviceLuid = 0;
					static_assert(sizeof(deviceLuid) == VK_LUID_SIZE);
					memcpy(&deviceLuid, id.deviceLUID, VK_LUID_SIZE);
					if (id.deviceLUIDValid && deviceLuid == luid) {
						*out = phy;
						return;
					}
				}
			}
		}
	}

	// VK_NULL_HANDLE (logged) if it can't be found: GetOutputDevice then answers 0, which a game takes as "any"
	TemporaryVk* vk = temporaryGraphics ? temporaryGraphics->GetAsVk() : nullptr;
	if (vk == nullptr) {
		OOVR_LOG("VkGetPhysicalDevice: no runtime answer, and not using a temporary Vulkan instance");
		return;
	}

	// Find the UUID of the physical device the temporary instance is running on
	VkPhysicalDeviceIDProperties idProps = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
	VkPhysicalDeviceProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idProps };
	vkGetPhysicalDeviceProperties2(vk->physicalDevice, &props);

	// Look through all the physical devices on the target instance and find the matching one
	uint32_t devCount = 0;
	if (vkEnumeratePhysicalDevices(instance, &devCount, nullptr) != VK_SUCCESS)
		return;
	std::vector<VkPhysicalDevice> physicalDevices(devCount);
	if (vkEnumeratePhysicalDevices(instance, &devCount, physicalDevices.data()) < 0)
		return;
	physicalDevices.resize(devCount);

	for (VkPhysicalDevice phy : physicalDevices) {
		VkPhysicalDeviceIDProperties devIdProps = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
		VkPhysicalDeviceProperties2 devProps = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &devIdProps };
		vkGetPhysicalDeviceProperties2(phy, &devProps);

		if (memcmp(devIdProps.deviceUUID, idProps.deviceUUID, sizeof(devIdProps.deviceUUID)) != 0)
			continue;

		// Found it
		*out = phy;
		return;
	}

	OOVR_LOG("Could not find matching Vulkan physical device for instance");
}

#endif
