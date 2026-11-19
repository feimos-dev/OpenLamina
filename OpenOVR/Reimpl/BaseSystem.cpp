#include "logging.h"
#include "stdafx.h"
#define BASE_IMPL
#include "BaseCompositor.h"
#include "BaseExtendedDisplay.h"
#include "BaseInput.h"
#include "BaseRenderModels.h"
#include "BaseOverlay.h"
#include "BaseSystem.h"
#include "Misc/ProgramEvents.h"
#include "Misc/SharedStore.h"
#include "Drivers/Backend.h"
#include "Misc/Config.h"
#include "Misc/EnumNames.h"
#include "Misc/Foveation.h"
#include "Misc/xrmoreutils.h"
#include "convert.h"
#include "generated/static_bases.gen.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cinttypes>
#include <string>

#ifdef SUPPORT_DX
#include <d3d9.h> // for GetD3D9AdapterIndex (loaded when asked, not linked)
#include <dxgi.h> // for GetDefaultAdapterLuid
#pragma comment(lib, "dxgi.lib")
#endif

// Needed for GetOutputDevice if Vulkan is enabled
#if defined(SUPPORT_VK)
#include "../../DrvOpenXR/XrBackend.h"
#ifndef OC_XR_PORT
#include "OVR_CAPI_Vk.h"
#endif
#endif

#include "Misc/xr_ext.h"

using namespace vr;

namespace {

class PropertyPrinter {
private:
	const vr::ETrackedDeviceProperty prop;
	const vr::TrackedDeviceIndex_t idx;

public:
	PropertyPrinter(vr::ETrackedDeviceProperty prop, vr::TrackedDeviceIndex_t idx, const char* type_name)
	    : prop(prop), idx(idx)
	{
		if (oovr_global_configuration.LogGetTrackedProperty())
			OOVR_LOGF("Requested %s property %u for device %u", type_name, prop, idx);
	}

#define DEF_PRINT_RESULT(type, specifier, expr)                                    \
	void print_result(type result)                                                 \
	{                                                                              \
		if (oovr_global_configuration.LogGetTrackedProperty())                     \
			OOVR_LOGF("dev: %u | prop: %u | result: " specifier, idx, prop, expr); \
	}

	DEF_PRINT_RESULT(bool, "%s", (result) ? "true" : "false")
	DEF_PRINT_RESULT(float, "%f", result)
	DEF_PRINT_RESULT(int32_t, "%" PRIi32, result)
	DEF_PRINT_RESULT(uint64_t, "%" PRIu64, result)
	DEF_PRINT_RESULT(char*, "%s", result ? result : "(no buffer)")

	void print_result(HmdMatrix34_t result)
	{
		if (!oovr_global_configuration.LogGetTrackedProperty())
			return;
		std::string matrix = "[";
		for (size_t i = 0; i < 3; i++) {
			matrix.append(" [");
			for (size_t j = 0; j < 4; j++) {
				matrix.append(std::to_string(result.m[i][j]));
				if (j < 3)
					matrix.append(", ");
			}
			matrix.push_back(']');
			if (i < 2)
				matrix.push_back(',');
		}
		OOVR_LOGF("dev: %u | prop: %u | result: %s", idx, prop, matrix.c_str());
	}

	// bytes is the size of the buffer the game gave; with no buffer only the size is known
	template <typename T>
	void print_result(const T* buffer, size_t bytes)
	{
		if (!oovr_global_configuration.LogGetTrackedProperty())
			return;
		if (!buffer) {
			OOVR_LOGF("dev: %u | prop: %u | result: no buffer", idx, prop);
			return;
		}
		std::string array = "{";
		for (size_t i = 0; i < bytes / sizeof(T); i++) {
			if (i)
				array.append(", ");
			array.append(std::to_string(buffer[i]));
		}
		array.push_back('}');
		OOVR_LOGF("dev: %u | prop: %u | result: %s", idx, prop, array.c_str());
	}
};

} // namespace

BaseSystem::BaseSystem()
{
	// The default origin is now set by the default value of currentSpace

	// Other programs' events from now on, and this program's events from calls into this queue
	SharedStore::Get().PollEvents(storeEventCursor);
	OcSetLocalEventSink([](const VREvent_t& e) {
		if (BaseSystem* system = GetUnsafeBaseSystem())
			system->_EnqueueEvent(e);
	});
}

BaseSystem::~BaseSystem()
{
	OcSetLocalEventSink(nullptr);
	// The origin is static (compositor and overlay calls may come before IVRSystem), but goes with the session:
	// a VR_Init after VR_Shutdown starts on the default again
	currentSpace = XR_REFERENCE_SPACE_TYPE_STAGE;
}

void BaseSystem::GetRecommendedRenderTargetSize(uint32_t* width, uint32_t* height)
{
	BackendManager::Instance().GetPrimaryHMD()->GetRecommendedRenderTargetSize(width, height);
}

HmdMatrix44_t BaseSystem::GetProjectionMatrix(EVREye eye, float znear, float zfar)
{
	return BackendManager::Instance().GetPrimaryHMD()->GetProjectionMatrix(eye, znear, zfar);
}

void BaseSystem::GetProjectionRaw(EVREye eye, float* pfLeft, float* pfRight, float* pfTop, float* pfBottom)
{
	return BackendManager::Instance().GetPrimaryHMD()->GetProjectionRaw(eye, pfLeft, pfRight, pfTop, pfBottom);
}

bool BaseSystem::ComputeDistortion(EVREye eEye, float fU, float fV, DistortionCoordinates_t* out)
{
	return BackendManager::Instance().GetPrimaryHMD()->ComputeDistortion(eEye, fU, fV, out);
}

bool BaseSystem::ComputeDistortionSet(EVREye eEye, EVRDistortionChannel eChannel, bool bAsNormalizedDeviceCoordinates,
    uint32_t nNumCoordinates, const DistortionCoordinate_t* pInput, DistortionCoordinate_t* pOutput)
{
	// Like ComputeDistortion: the OpenXR runtime does the distortion, so the coordinates come back as given
	if (nNumCoordinates == 0)
		return true;
	if (!pInput || !pOutput)
		return false;
	std::copy(pInput, pInput + nNumCoordinates, pOutput);
	return true;
}

HmdMatrix34_t BaseSystem::GetEyeToHeadTransform(EVREye ovr_eye)
{
	return BackendManager::Instance().GetPrimaryHMD()->GetEyeToHeadTransform(ovr_eye);
}

bool BaseSystem::GetTimeSinceLastVsync(float* pfSecondsSinceLastVsync, uint64_t* pulFrameCounter)
{
	return BackendManager::Instance().GetPrimaryHMD()->GetTimeSinceLastVsync(pfSecondsSinceLastVsync, pulFrameCounter);
}

bool BaseSystem::_RuntimeAdapterLuid(uint64_t* luid)
{
#ifdef SUPPORT_DX
	if (!xr_ext || xr_instance == XR_NULL_HANDLE)
		return false;
	auto pack = [luid](const LUID& l) {
		*luid = (uint64_t)l.LowPart | ((uint64_t)(uint32_t)l.HighPart << 32);
		return true;
	};
#if defined(SUPPORT_DX11)
	if (xr_ext->xrGetD3D11GraphicsRequirementsKHR_Available()) {
		XrGraphicsRequirementsD3D11KHR requirements = { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
		if (XR_SUCCEEDED(xr_ext->xrGetD3D11GraphicsRequirementsKHR(xr_instance, xr_system, &requirements)))
			return pack(requirements.adapterLuid);
	}
#endif
#if defined(SUPPORT_DX12)
	if (xr_ext->xrGetD3D12GraphicsRequirementsKHR_Available()) {
		XrGraphicsRequirementsD3D12KHR requirements = { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR };
		if (XR_SUCCEEDED(xr_ext->xrGetD3D12GraphicsRequirementsKHR(xr_instance, xr_system, &requirements)))
			return pack(requirements.adapterLuid);
	}
#endif
#endif
	(void)luid;
	return false;
}

int32_t BaseSystem::_DxgiAdapterIndexOf(uint64_t luid)
{
#ifdef SUPPORT_DX
	IDXGIFactory* factory = nullptr;
	if (FAILED(CreateDXGIFactory(IID_PPV_ARGS(&factory))) || !factory)
		return -1;
	int32_t index = -1;
	IDXGIAdapter* adapter = nullptr;
	// Bounded by the adapter count (EnumAdapters ends with DXGI_ERROR_NOT_FOUND)
	for (UINT i = 0; index < 0 && factory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
		DXGI_ADAPTER_DESC desc;
		if (SUCCEEDED(adapter->GetDesc(&desc))
		    && ((uint64_t)desc.AdapterLuid.LowPart | ((uint64_t)(uint32_t)desc.AdapterLuid.HighPart << 32)) == luid)
			index = (int32_t)i;
		adapter->Release();
	}
	factory->Release();
	return index;
#else
	(void)luid;
	return -1;
#endif
}

int32_t BaseSystem::GetD3D9AdapterIndex()
{
	// The D3D9Ex adapter with the OpenXR runtime's adapter's LUID, else the first; -1 where there's no way to tell
#ifdef SUPPORT_DX
	uint64_t runtimeLuid = 0;
	if (!_RuntimeAdapterLuid(&runtimeLuid))
		return -1;

	HMODULE d3d9 = LoadLibraryW(L"d3d9.dll");
	if (!d3d9)
		return -1;
	using Create9Ex = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
	auto create = (Create9Ex)GetProcAddress(d3d9, "Direct3DCreate9Ex");
	IDirect3D9Ex* d3d = nullptr;
	int32_t index = -1;
	if (create && SUCCEEDED(create(D3D_SDK_VERSION, &d3d)) && d3d) {
		index = 0;
		for (UINT i = 0; i < d3d->GetAdapterCount(); i++) {
			LUID luid = {};
			if (SUCCEEDED(d3d->GetAdapterLUID(i, &luid))
			    && ((uint64_t)luid.LowPart | ((uint64_t)(uint32_t)luid.HighPart << 32)) == runtimeLuid) {
				index = (int32_t)i;
				break;
			}
		}
		d3d->Release();
	}
	FreeLibrary(d3d9);
	return index;
#else
	return -1;
#endif
}

void BaseSystem::GetDXGIOutputInfo(int32_t* pnAdapterIndex, int32_t* pnAdapterOutputIndex)
{
	GetDXGIOutputInfo(pnAdapterIndex);
	if (pnAdapterOutputIndex)
		*pnAdapterOutputIndex = pnAdapterIndex && *pnAdapterIndex < 0 ? -1 : 0;
}

void BaseSystem::GetDXGIOutputInfo(int32_t* adapterIndex)
{
	if (!adapterIndex)
		return;
	// The adapter the OpenXR runtime names; -1 (an error) where it names none
	*adapterIndex = -1;
#ifdef SUPPORT_DX
	uint64_t luid = 0;
	OC_SOFT_CHECK_VOID(_RuntimeAdapterLuid(&luid), "GetDXGIOutputInfo: the OpenXR runtime names no D3D adapter");
	int32_t index = _DxgiAdapterIndexOf(luid);
	OC_SOFT_CHECK_VOID(index >= 0, "GetDXGIOutputInfo: no adapter has the OpenXR runtime's LUID");
	*adapterIndex = index;
#else
	OC_UNSUPPORTED_VOID("GetDXGIOutputInfo: this build has no D3D support");
#endif
}

void BaseSystem::GetOutputDevice(uint64_t* pnDevice, ETextureType textureType, VkInstance_T* pInstance)
{
	if (!pnDevice)
		return;
	*pnDevice = 0;

	switch (textureType) {
	case TextureType_DirectX:
	case TextureType_DirectX12: {
		// The runtime's adapter's LUID
		uint64_t luid = 0;
		if (_RuntimeAdapterLuid(&luid))
			*pnDevice = luid;
		else
			OOVR_LOG_ONCE("GetOutputDevice: the OpenXR runtime names no D3D adapter, answering 0");
		break;
	}
	case TextureType_Vulkan: {
#if defined(SUPPORT_VK)
		VkPhysicalDevice physDev;
		XrBackend::VkGetPhysicalDevice(pInstance, &physDev);
		*pnDevice = (uint64_t)physDev;
#else
		OC_UNSUPPORTED_VOID("GetOutputDevice: this build has no Vulkan support");
#endif
		break;
	}
	default:
		OOVR_LOGF("Unsupported texture type for GetOutputDevice %d", textureType);
	}
}

bool BaseSystem::IsDisplayOnDesktop()
{
	return false; // Always in direct mode
}

bool BaseSystem::SetDisplayVisibility(bool bIsVisibleOnDesktop)
{
	return false; // Always render in direct mode
}

void BaseSystem::GetDeviceToAbsoluteTrackingPose(ETrackingUniverseOrigin toOrigin, float predictedSecondsToPhotonsFromNow,
    TrackedDevicePose_t* poseArray, uint32_t poseArrayCount)
{

	BackendManager::Instance().GetDeviceToAbsoluteTrackingPose(toOrigin, predictedSecondsToPhotonsFromNow, poseArray, poseArrayCount);
}

HmdMatrix34_t BaseSystem::GetSeatedZeroPoseToStandingAbsoluteTrackingPose()
{
	auto sessionLock = xr_session.lock_shared(); // held while reading: a session restart waits for it
	glm::mat4 m = glm::identity<glm::mat4>();
	XrSpaceLocation location{ XR_TYPE_SPACE_LOCATION, nullptr, 0, {} };
	if (!xr_gbl)
		return G2S_m34(glm::identity<glm::mat4>());

	OOVR_FAILED_XR_SOFT_ABORT(xrLocateSpace(xr_gbl->seatedSpace, xr_gbl->floorSpace, xr_gbl->GetBestTime(), &location));

	if ((location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) && (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
		m = glm::mat4(X2G_quat(location.pose.orientation));
		m[3] = glm::vec4(X2G_v3f(location.pose.position), 1.0f);
	}

	return G2S_m34(m);
}

HmdMatrix34_t BaseSystem::GetRawZeroPoseToStandingAbsoluteTrackingPose()
{
	// The raw space is mapped to the stage space, so the two coincide
	return G2S_m34(glm::identity<glm::mat4>());
}

void BaseSystem::SortRightToLeft(std::vector<DeviceSortKey>& devices)
{
	// Devices without a position keep their order, after the ones that have one
	std::stable_sort(devices.begin(), devices.end(), [](const DeviceSortKey& a, const DeviceSortKey& b) {
		if (a.hasPosition != b.hasPosition)
			return a.hasPosition;
		return a.hasPosition && a.right > b.right;
	});
}

uint32_t BaseSystem::GetSortedTrackedDeviceIndicesOfClass(ETrackedDeviceClass targetClass,
    vr::TrackedDeviceIndex_t* indexArray, uint32_t indexCount,
    vr::TrackedDeviceIndex_t unRelativeToTrackedDeviceIndex)
{
	if (!indexArray)
		indexCount = 0;

	// The reference: a device's own frame, or the standing space when it isn't one
	glm::mat4 toLocal = glm::identity<glm::mat4>();
	std::shared_ptr<ITrackedDevice> reference = BackendManager::Instance().GetDevice(unRelativeToTrackedDeviceIndex);
	if (reference) {
		vr::TrackedDevicePose_t pose = {};
		reference->GetPose(vr::TrackingUniverseStanding, &pose, TrackingStateType_Now);
		if (pose.bPoseIsValid)
			toLocal = glm::inverse(S2G_m34(pose.mDeviceToAbsoluteTracking));
	}

	std::vector<DeviceSortKey> found;
	for (vr::TrackedDeviceIndex_t dev = 0; dev < vr::k_unMaxTrackedDeviceCount; dev++) {
		if (GetTrackedDeviceClass(dev) != targetClass)
			continue;

		DeviceSortKey key{ dev, false, 0.0f };
		std::shared_ptr<ITrackedDevice> device = BackendManager::Instance().GetDevice(dev);
		if (device) {
			vr::TrackedDevicePose_t pose = {};
			device->GetPose(vr::TrackingUniverseStanding, &pose, TrackingStateType_Now);
			if (pose.bPoseIsValid) {
				key.hasPosition = true;
				key.right = (toLocal * S2G_m34(pose.mDeviceToAbsoluteTracking))[3].x;
			}
		}
		found.push_back(key);
	}

	SortRightToLeft(found);

	for (uint32_t i = 0; i < found.size() && i < indexCount; i++)
		indexArray[i] = found[i].index;

	return (uint32_t)found.size();
}

EDeviceActivityLevel BaseSystem::GetTrackedDeviceActivityLevel(vr::TrackedDeviceIndex_t unDeviceId)
{
	// TODO implement
	return k_EDeviceActivityLevel_UserInteraction;
}

void BaseSystem::ApplyTransform(TrackedDevicePose_t* pOutputPose, const TrackedDevicePose_t* pTrackedDevicePose, const HmdMatrix34_t* pTransform)
{
	if (!pOutputPose || !pTrackedDevicePose || !pTransform)
		return;
	// As SteamVR does: the pose is moved by the transform, and its velocities turned by the transform's rotation
	TrackedDevicePose_t out = *pTrackedDevicePose;
	glm::mat4 transform = S2G_m34(*pTransform);
	out.mDeviceToAbsoluteTracking = G2S_m34(transform * S2G_m34(pTrackedDevicePose->mDeviceToAbsoluteTracking));
	glm::mat3 rotation(transform);
	out.vVelocity = G2S_v3f(rotation * S2G_v3f(pTrackedDevicePose->vVelocity));
	out.vAngularVelocity = G2S_v3f(rotation * S2G_v3f(pTrackedDevicePose->vAngularVelocity));
	*pOutputPose = out;
}

void BaseSystem::ApplyTransform(TrackedDevicePose_t* pOutputPose, const TrackedDevicePose_t& trackedDevicePose, const HmdMatrix34_t& transform)
{
	ApplyTransform(pOutputPose, &trackedDevicePose, &transform);
}

vr::TrackedDeviceIndex_t BaseSystem::GetTrackedDeviceIndexForControllerRole(vr::ETrackedControllerRole unDeviceType)
{
	if (unDeviceType == TrackedControllerRole_Invalid)
		return vr::k_unTrackedDeviceIndexInvalid;

	for (vr::TrackedDeviceIndex_t index = 0; index < vr::k_unMaxTrackedDeviceCount; index++) {
		std::shared_ptr<ITrackedDevice> dev = BackendManager::Instance().GetDevice(index);
		if (dev && dev->GetControllerRole() == unDeviceType)
			return index;
	}

	return vr::k_unTrackedDeviceIndexInvalid; // This is what SteamVR does for unknown devices
}

vr::ETrackedControllerRole BaseSystem::GetControllerRoleForTrackedDeviceIndex(vr::TrackedDeviceIndex_t unDeviceIndex)
{
	std::shared_ptr<ITrackedDevice> dev = BackendManager::Instance().GetDevice(unDeviceIndex);
	if (!dev)
		return TrackedControllerRole_Invalid;

	return dev->GetControllerRole();
}

ETrackedDeviceClass BaseSystem::GetTrackedDeviceClass(vr::TrackedDeviceIndex_t deviceIndex)
{
	if (deviceIndex >= vr::k_unMaxTrackedDeviceCount)
		return TrackedDeviceClass_Invalid;

	// A slot that held a device keeps its class once the device is gone
	std::shared_ptr<ITrackedDevice> dev = BackendManager::Instance().GetDevice(deviceIndex);
	if (dev) {
		ETrackedDeviceClass cls = dev->GetTrackedDeviceClass();
		slotClasses[deviceIndex] = (int32_t)cls;
		return cls;
	}

	return (ETrackedDeviceClass)slotClasses[deviceIndex].load();
}

bool BaseSystem::IsTrackedDeviceConnected(vr::TrackedDeviceIndex_t deviceIndex)
{
	return BackendManager::Instance().GetDevice(deviceIndex) != nullptr;
}

bool BaseSystem::GetBoolTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError* pErrorL)
{
	PropertyPrinter p(prop, unDeviceIndex, "bool");
	std::shared_ptr<ITrackedDevice> dev = BackendManager::Instance().GetDevice(unDeviceIndex);

	if (!dev) {
		if (pErrorL)
			*pErrorL = TrackedProp_InvalidDevice;
		return false;
	}

	bool ret = dev->GetBoolTrackedDeviceProperty(prop, pErrorL);
	p.print_result(ret);
	return ret;
}

float BaseSystem::GetFloatTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError* pErrorL)
{
	PropertyPrinter p(prop, unDeviceIndex, "float");
	std::shared_ptr<ITrackedDevice> dev = BackendManager::Instance().GetDevice(unDeviceIndex);

	if (!dev) {
		if (pErrorL)
			*pErrorL = TrackedProp_InvalidDevice;
		return 0;
	}

	float ret = dev->GetFloatTrackedDeviceProperty(prop, pErrorL);
	p.print_result(ret);
	return ret;
}

int32_t BaseSystem::GetInt32TrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError* pErrorL)
{
	PropertyPrinter p(prop, unDeviceIndex, "int32_t");
	std::shared_ptr<ITrackedDevice> dev = BackendManager::Instance().GetDevice(unDeviceIndex);

	if (!dev) {
		if (pErrorL)
			*pErrorL = TrackedProp_InvalidDevice;
		return 0;
	}

	int32_t ret = dev->GetInt32TrackedDeviceProperty(prop, pErrorL);
	p.print_result(ret);
	return ret;
}

uint64_t BaseSystem::GetUint64TrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError* pErrorL)
{
	PropertyPrinter p(prop, unDeviceIndex, "uint64_t");
	std::shared_ptr<ITrackedDevice> dev = BackendManager::Instance().GetDevice(unDeviceIndex);

	if (!dev) {
		if (pErrorL)
			*pErrorL = TrackedProp_InvalidDevice;
		return 0;
	}

	uint64_t ret = dev->GetUint64TrackedDeviceProperty(prop, pErrorL);
	p.print_result(ret);
	return ret;
}

HmdMatrix34_t BaseSystem::GetMatrix34TrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError* pErrorL)
{
	PropertyPrinter p(prop, unDeviceIndex, "HmdMatrix34_t");
	std::shared_ptr<ITrackedDevice> dev = BackendManager::Instance().GetDevice(unDeviceIndex);

	if (!dev) {
		if (pErrorL)
			*pErrorL = TrackedProp_InvalidDevice;
		return {};
	}

	HmdMatrix34_t ret = dev->GetMatrix34TrackedDeviceProperty(prop, pErrorL);
	p.print_result(ret);
	return ret;
}

uint32_t BaseSystem::GetArrayTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, PropertyTypeTag_t propType, void* pBuffer, uint32_t unBufferSize, ETrackedPropertyError* pError)
{
	PropertyPrinter p(prop, unDeviceIndex, "array");
	std::shared_ptr<ITrackedDevice> dev = BackendManager::Instance().GetDevice(unDeviceIndex);

	if (!dev) {
		if (pError)
			*pError = TrackedProp_InvalidDevice;
		return 0;
	}

	uint32_t ret = dev->GetArrayTrackedDeviceProperty(prop, propType, pBuffer, unBufferSize, pError);
	if (oovr_global_configuration.LogGetTrackedProperty()) {
#define ARRAY_CASE(tag_type, actual_type)                         \
	case k_un##tag_type##PropertyTag: {                           \
		actual_type* buffer = static_cast<actual_type*>(pBuffer); \
		p.print_result(buffer, ret <= unBufferSize ? ret : 0);    \
	} break
		switch (propType) {
			ARRAY_CASE(Float, float);
			ARRAY_CASE(Int32, int32_t);
			ARRAY_CASE(Uint64, uint64_t);
			ARRAY_CASE(Bool, bool);
			ARRAY_CASE(Double, double);
		default:
			OOVR_LOGF("WARNING: Was not able to log array with PropertyTypeTag %" PRIu32 "!", propType);
			break;
		}
	}
	return ret;
}

uint32_t BaseSystem::GetStringTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop,
    VR_OUT_STRING() char* value, uint32_t bufferSize, ETrackedPropertyError* pErrorL)
{
	PropertyPrinter p(prop, unDeviceIndex, "string");
	std::shared_ptr<ITrackedDevice> dev = BackendManager::Instance().GetDevice(unDeviceIndex);

	if (!dev) {
		if (pErrorL)
			*pErrorL = TrackedProp_InvalidDevice;
		return 0;
	}

	uint32_t ret = dev->GetStringTrackedDeviceProperty(prop, value, bufferSize, pErrorL);
	p.print_result(value);
	return ret;
}

const char* BaseSystem::GetPropErrorNameFromEnum(ETrackedPropertyError error)
{
	// As SteamVR answers
	return OC_EnumNameAsSteamVR("ETrackedPropertyError", (int32_t)error, "Unknown property error (%d)");
}

bool BaseSystem::IsInputAvailable()
{
	return BackendManager::Instance().IsInputAvailable();
}

bool BaseSystem::IsSteamVRDrawingControllers()
{
	// Not before the first frame is shown: a game that stops submitting while it's told to pause never gets there
	return BackendManager::Instance().IsInputFocusLost();
}

bool BaseSystem::ShouldApplicationPause()
{
	return BackendManager::Instance().IsInputFocusLost();
}

bool BaseSystem::ShouldApplicationReduceRenderingWork()
{
	return BackendManager::Instance().ShouldReduceRenderingWork();
}

void BaseSystem::_OnPostFrame()
{
	frameNumber++;

	// Note: OpenXR event handling is now in XrBackend

	// Create the input system, if the game hasn't already done so
	// See the comment in GetControllerState for the rationale here
	if (!inputSystem) {
		inputSystem = GetBaseInput();

		if (!inputSystem) {
			inputSystem = GetCreateBaseInput();

			// We used to load an empty manifest here. Instead, we now wait until games first read the legacy
			// inputs. This way games that wait until several frames in to load their manifest will still work.
		}
	}

	if (inputSystem) {
		inputSystem->InternalUpdate();

		// The legacy button events, once the game reads the legacy input (asking for the controller state before
		// then would load the empty manifest, and a game's own manifest after that couldn't be bound)
		if (inputSystem->IsUsingLegacyInput()) {
			CheckControllerEvents(leftHandIndex, lastLegacyStates[0]);
			CheckControllerEvents(rightHandIndex, lastLegacyStates[1]);
		}
	}
}

void BaseSystem::PushEventLocked(const event_info_t& e)
{
	if (events.size() >= maxQueuedEvents) {
		OOVR_LOG_ONCEF("The event queue is full (%zu events, the program isn't polling): dropping the oldest", maxQueuedEvents);
		events.pop();
	}
	events.push(e);
}

void BaseSystem::_EnqueueEvent(const VREvent_t& e)
{
	if (e.trackedDeviceIndex < announcedDevices.size()) {
		if (e.eventType == VREvent_TrackedDeviceActivated)
			announcedDevices[e.trackedDeviceIndex] = true;
		else if (e.eventType == VREvent_TrackedDeviceDeactivated)
			announcedDevices[e.trackedDeviceIndex] = false;
	}
	std::unique_lock lock(events_mutex);
	PushEventLocked(e);
}

void BaseSystem::AnnounceConnectedDevices()
{
	for (vr::TrackedDeviceIndex_t i = 0; i < announcedDevices.size(); i++) {
		if (announcedDevices[i] || !IsTrackedDeviceConnected(i))
			continue;
		VREvent_t e{};
		e.eventType = VREvent_TrackedDeviceActivated;
		e.trackedDeviceIndex = i;
		_EnqueueEvent(e);
	}
}

void BaseSystem::_PostEvent(EVREventType type, TrackedDeviceIndex_t device, const VREvent_Data_t& data, bool alsoToOtherPrograms)
{
	OcPostEvent(type, device, data, alsoToOtherPrograms);
}

void BaseSystem::_BlockInputsUntilReleased()
{
	blockingInputsUntilRelease[0] = true;
	blockingInputsUntilRelease[1] = true;
}

float BaseSystem::SGetIpd()
{
	std::shared_ptr<IHMD> dev = BackendManager::Instance().GetPrimaryHMD();
	float ipd = dev->GetIPD();
	static float lastIpd = NAN;
	if (std::isnan(lastIpd) || std::fabs(ipd - lastIpd) > 1e-5) {
		lastIpd = ipd;
		OOVR_LOGF("IPD: %f", ipd);
	}
	return ipd;
}

void BaseSystem::CheckControllerEvents(TrackedDeviceIndex_t hand, VRControllerState_t& last)
{
	VRControllerState_t state;
	GetControllerState(hand, &state, sizeof(state));

	if (state.ulButtonPressed == last.ulButtonPressed && state.ulButtonTouched == last.ulButtonTouched) {
		// Nothing has changed
		// Though still update other properties in the state
		last = state;

		return;
	}

	VREvent_t ev_base{};
	ev_base.trackedDeviceIndex = hand;
	ev_base.eventAgeSeconds = 0; // TODO
	ev_base.data.controller = { 0 };

	TrackedDevicePose_t pose{};
	BaseCompositor* compositor = GetUnsafeBaseCompositor();
	if (compositor) {
		compositor->GetSinglePoseRendering(compositor->GetTrackingSpace(), hand, &pose);
	}

	// Check each possible button, and fire an event if it changed
	// (note that incrementing enums in C++ is a bit of a pain, wrt the casting)
	for (EVRButtonId id = k_EButton_ApplicationMenu; id < k_EButton_Max; id = (EVRButtonId)(id + 1)) {
		ev_base.data.controller.button = id;
		uint64_t mask = ButtonMaskFromId(id);

		// Was the button pressed or released?
		bool oldState = (last.ulButtonPressed & mask) != 0;
		bool newState = (state.ulButtonPressed & mask) != 0;

		if (newState != oldState) {
			VREvent_t e = ev_base;
			e.eventType = newState ? VREvent_ButtonPress : VREvent_ButtonUnpress;
			std::unique_lock lock(events_mutex);
			PushEventLocked(event_info_t(e, pose));
		}

		// Did the user touch or break contact with the button?
		oldState = (last.ulButtonTouched & mask) != 0;
		newState = (state.ulButtonTouched & mask) != 0;

		if (newState != oldState) {
			VREvent_t e = ev_base;
			e.eventType = newState ? VREvent_ButtonTouch : VREvent_ButtonUntouch;
			std::unique_lock lock(events_mutex);
			PushEventLocked(event_info_t(e, pose));
		}
	}

	last = state;
}

bool BaseSystem::PollNextEvent(VREvent_t* pEvent, uint32_t uncbVREvent)
{
	return PollNextEventWithPose(TrackingUniverseStanding, pEvent, uncbVREvent, NULL);
}

bool BaseSystem::PollNextEventWithPose(ETrackingUniverseOrigin eOrigin, VREvent_t* pEvent, uint32_t uncbVREvent, vr::TrackedDevicePose_t* pTrackedDevicePose)
{
	// No event struct, or one too short to say what the event is: nothing is taken off the queue
	if (!pEvent || uncbVREvent < offsetof(VREvent_t, data))
		return false;
	memset(pEvent, 0, uncbVREvent);

	// The devices there already are when the program first asks: it builds its device list from their events
	if (!startupDevicesAnnounced.exchange(true))
		AnnounceConnectedDevices();

	// Other programs' events first join the queue
	std::vector<StoreEvent> others = SharedStore::Get().PollEvents(storeEventCursor);

	std::unique_lock lock(events_mutex);
	for (const StoreEvent& s : others) {
		// OpenLamina's own (a pointer on an overlay) are taken by those they're for, never the game
		if (!OcIsPrivateEvent(s.eventType))
			PushEventLocked(OcUnpackEvent(s.eventType, s.data));
	}

	if (events.empty()) {
		return false;
	}

	event_info_t info = events.front();
	VREvent_t e = info.ev;
	events.pop();

	memcpy(pEvent, &e, std::min((size_t)uncbVREvent, sizeof(e)));

	if (pTrackedDevicePose) {
		*pTrackedDevicePose = info.pose;
	}

	return true;
}

const char* BaseSystem::GetEventTypeNameFromEnum(EVREventType eType)
{
	// As SteamVR answers
	return OC_EnumNameAsSteamVR("EVREventType", (int32_t)eType, "Unknown EVREventType (%d)");
}

HiddenAreaMesh_t BaseSystem::GetHiddenAreaMesh(EVREye eEye, EHiddenAreaMeshType type)
{
	return BackendManager::Instance().GetPrimaryHMD()->GetHiddenAreaMesh(eEye, type);
}

bool BaseSystem::GetControllerState(vr::TrackedDeviceIndex_t controllerDeviceIndex, vr::VRControllerState_t* controllerState, uint32_t controllerStateSize)
{
	if (!controllerState)
		return false;
	if (sizeof(VRControllerState_t) != controllerStateSize) {
		// Zeroed within the size the app gave, so it reads "no buttons" whatever its struct is
		memset(controllerState, 0, std::min<size_t>(controllerStateSize, sizeof(VRControllerState_t)));
		OC_UNSUPPORTED(false, "GetControllerState: a VRControllerState_t of %u bytes, not %u", controllerStateSize,
		    (unsigned)sizeof(VRControllerState_t));
	}

	memset(controllerState, 0, controllerStateSize);

	// The headset's proximity sensor, while the runtime says whether the user wears it
	if (controllerDeviceIndex == k_unTrackedDeviceIndex_Hmd && xr_gbl && xr_gbl->supportsUserPresence) {
		std::shared_ptr<ITrackedDevice> hmd = BackendManager::Instance().GetDevice(k_unTrackedDeviceIndex_Hmd);
		return hmd && hmd->GetControllerState(controllerState);
	}

	if (!inputSystem) {
		inputSystem = GetBaseInput();
	}

	// Since we can only bind the manifest once, and some games may never call it or may
	//  try reading the controller state first, wait until the first frame has been submitted (returning
	//  blank controller states until then) AND the game tries reading the controller state.
	// This will work for both games that call this function before loading their manifest (do they really
	//  exist? The comments mentioned that but it seems unlikely one ever actually did, and this bit may
	//  be unnecessary.) and for games that load their manifest late (Jet Island does as of 29/05/2022).
	// This won't work for games that need hand positions but don't use either legacy or new input, but
	//  it seems unlikely any shipping game falls into that category.
	if (!inputSystem) {
		return true;
	}
	if (frameNumber > 0)
		inputSystem->LoadEmptyManifestIfRequired();

	const bool result = inputSystem->GetLegacyControllerState(controllerDeviceIndex, controllerState);
	// Held by OpenLamina's dashboard: read (the dashboard's button is in it) and zeroed
	if (BaseInput::HeldByDashboard()) {
		const uint32_t packet = controllerState->unPacketNum;
		memset(controllerState, 0, controllerStateSize);
		controllerState->unPacketNum = packet;
	}
	return result;
}

bool BaseSystem::GetControllerStateWithPose(ETrackingUniverseOrigin eOrigin, vr::TrackedDeviceIndex_t unControllerDeviceIndex,
    vr::VRControllerState_t* pControllerState, uint32_t unControllerStateSize, TrackedDevicePose_t* pTrackedDevicePose)
{
	if (pTrackedDevicePose)
		BackendManager::Instance().GetSinglePose(eOrigin, unControllerDeviceIndex, pTrackedDevicePose, ETrackingStateType::TrackingStateType_Now);

	return GetControllerState(unControllerDeviceIndex, pControllerState, unControllerStateSize);
}

void BaseSystem::TriggerHapticPulse(vr::TrackedDeviceIndex_t unControllerDeviceIndex, uint32_t unAxisId, unsigned short usDurationMicroSec)
{
	if (!oovr_global_configuration.Haptics())
		return;

	if (unControllerDeviceIndex == leftHandIndex || unControllerDeviceIndex == rightHandIndex) {

		// Similar to GetControllerState, wait until the input system is ready
		if (!inputSystem) {
			return;
		}

		inputSystem->TriggerLegacyHapticPulse(unControllerDeviceIndex, (uint64_t)usDurationMicroSec * 1000);
	}
	// if index is invalid, nothing to do
	return;
}

const char* BaseSystem::GetButtonIdNameFromEnum(EVRButtonId eButtonId)
{
	// As SteamVR answers
	return OC_EnumNameAsSteamVR("EVRButtonId", (int32_t)eButtonId, "Unknown EVRButtonId (%d)");
}

const char* BaseSystem::GetControllerAxisTypeNameFromEnum(EVRControllerAxisType eAxisType)
{
	// As SteamVR answers
	return OC_EnumNameAsSteamVR("EVRControllerAxisType", (int32_t)eAxisType, "Unknown EVRControllerAxisType (%d)");
}

bool BaseSystem::CaptureInputFocus()
{
	// Retired in SteamVR: nothing takes the input focus from other programs
	return false;
}

void BaseSystem::ReleaseInputFocus()
{
	// CaptureInputFocus never takes it
}

bool BaseSystem::IsInputFocusCapturedByAnotherProcess()
{
	return !IsInputAvailable();
}

uint32_t BaseSystem::DriverDebugRequest(vr::TrackedDeviceIndex_t unDeviceIndex, const char* pchRequest, char* pchResponseBuffer, uint32_t unResponseBufferSize)
{
	if (pchResponseBuffer && unResponseBufferSize)
		pchResponseBuffer[0] = 0;
	OC_UNSUPPORTED(0, "DriverDebugRequest: there are no OpenVR drivers to ask");
}

vr::EVRFirmwareError BaseSystem::PerformFirmwareUpdate(vr::TrackedDeviceIndex_t unDeviceIndex)
{
	OC_UNSUPPORTED(vr::VRFirmwareError_Fail, "PerformFirmwareUpdate: firmware updates are the headset vendor's tools'");
}

void BaseSystem::AcknowledgeQuit_Exiting()
{
	// Nothing waits for it: OpenLamina never ends a program
}

void BaseSystem::AcknowledgeQuit_UserPrompt()
{
	// Retired in SteamVR: nothing waits for it
}

uint32_t BaseSystem::GetAppContainerFilePaths(VR_OUT_STRING() char* pchBuffer, uint32_t unBufferSize)
{
	if (pchBuffer && unBufferSize)
		pchBuffer[0] = 0;
	// No app containers (Linux's pressure-vessel): no paths
	return 0;
}

const char* BaseSystem::GetRuntimeVersion()
{
	return "1.16.8";
}

DistortionCoordinates_t BaseSystem::ComputeDistortion(EVREye eEye, float fU, float fV)
{
	// IVRSystem_003-008 return the coordinates, with no way to say there are none: the OpenXR runtime distorts the
	// frames itself, so they're the ones given, unless the HMD has others (it left them uninitialised before)
	DistortionCoordinates_t out = { { fU, fV }, { fU, fV }, { fU, fV } };
	DistortionCoordinates_t computed = out;
	if (ComputeDistortion(eEye, fU, fV, &computed))
		out = computed;
	return out;
}

HmdMatrix44_t BaseSystem::GetProjectionMatrix(EVREye eye, float znear, float zfar, EGraphicsAPIConvention convention)
{
	return BackendManager::Instance().GetPrimaryHMD()->GetProjectionMatrix(eye, znear, zfar, convention);
}

void BaseSystem::PerformanceTestEnableCapture(bool bEnable)
{
	// SteamVR's performance test isn't there
}

void BaseSystem::PerformanceTestReportFidelityLevelChange(int nFidelityLevel)
{
	// SteamVR's performance test isn't there
}

// Tracking origin stuff
void BaseSystem::ResetSeatedZeroPose()
{
	// Exclusive: the seated space is replaced, and other threads locate against it
	auto sessionLock = xr_session.lock();
	if (BackendManager::Instance().IsGraphicsConfigured() && xr_gbl) {
		XrSpaceVelocity velocity{ XR_TYPE_SPACE_VELOCITY };
		XrSpaceLocation location{ XR_TYPE_SPACE_LOCATION, &velocity, 0, {} };

		OOVR_FAILED_XR_SOFT_ABORT(xrLocateSpace(xr_gbl->viewSpace, xr_gbl->seatedSpace, xr_gbl->GetBestTime(), &location));

		if ((location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) && (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
			// The origin goes to the head's place now, with only the heading kept
			XrReferenceSpaceCreateInfo spaceInfo{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr, XR_REFERENCE_SPACE_TYPE_LOCAL,
				xr_utils::SeatedOriginAtHead(xr_gbl->seatedPose, location.pose) };

			// On failure the old seated space stays
			XrSpace newSpace = XR_NULL_HANDLE;
			OC_XR_SOFT_VOID(xrCreateReferenceSpace(xr_session.get(), &spaceInfo, &newSpace));
			xrDestroySpace(xr_gbl->seatedSpace);
			xr_gbl->seatedSpace = newSpace;
			xr_gbl->seatedPose = spaceInfo.poseInReferenceSpace;
			xr_seated_origin = xr_gbl->seatedPose;
		}
	}

	// As SteamVR, whether or not the pose could be found
	_PostEvent(VREvent_SeatedZeroPoseReset, k_unTrackedDeviceIndex_Hmd, VREvent_Data_t{}, false);
}

namespace {
// VREvent_t as SDKs 0.9.0 to 0.9.14 laid it out (the data union was 16 bytes: two uint64s), packed as the SDK packs its
// structs: 8 on Windows, 4 on Linux and macOS, where it's 28 bytes
#if defined(__linux__) || defined(__APPLE__)
#pragma pack(push, 4)
#else
#pragma pack(push, 8)
#endif
struct VREvent_0914_t {
	uint32_t eventType;
	vr::TrackedDeviceIndex_t trackedDeviceIndex;
	uint64_t data[2];
	float eventAgeSeconds;
};
#pragma pack(pop)
static_assert(offsetof(VREvent_0914_t, data) == 8, "0.9.x events have their data at 8");
static_assert(offsetof(VREvent_0914_t, eventAgeSeconds) == 24, "0.9.x events have their age at 24");
#if defined(__linux__) || defined(__APPLE__)
static_assert(sizeof(VREvent_0914_t) == 28, "0.9.x events are 28 bytes on Linux");
#else
static_assert(sizeof(VREvent_0914_t) == 32, "0.9.x events are 32 bytes on Windows");
#endif
} // namespace

bool BaseSystem::PollNextEventOld(ETrackingUniverseOrigin eOrigin, VREvent_t* pEvent, vr::TrackedDevicePose_t* pTrackedDevicePose)
{
	if (!pEvent)
		return false;
	memset(pEvent, 0, sizeof(VREvent_0914_t));
	static_assert(sizeof(VREvent_0914_t) == kOldEventSize, "the old event's size");

	VREvent_t event = {};
	if (!PollNextEventWithPose(eOrigin, &event, sizeof(event), pTrackedDevicePose))
		return false;
	WriteOldEvent(event, pEvent);
	return true;
}

void BaseSystem::WriteOldEvent(const VREvent_t& event, VREvent_t* pOld)
{
	VREvent_0914_t old = {};
	old.eventType = event.eventType;
	old.trackedDeviceIndex = event.trackedDeviceIndex;
	// Each event's data starts at the union's start in both, so the first 16 bytes are the old union's
	memcpy(old.data, &event.data, sizeof(old.data));
	old.eventAgeSeconds = event.eventAgeSeconds;
	memcpy(pOld, &old, sizeof(old));
}

void BaseSystem::GetWindowBounds(int32_t* pnX, int32_t* pnY, uint32_t* pnWidth, uint32_t* pnHeight)
{
	GetCreateBaseExtendedDisplay()->GetWindowBounds(pnX, pnY, pnWidth, pnHeight);
}

void BaseSystem::GetEyeOutputViewport(EVREye eEye, uint32_t* pnX, uint32_t* pnY, uint32_t* pnWidth, uint32_t* pnHeight)
{
	GetCreateBaseExtendedDisplay()->GetEyeOutputViewport(eEye, pnX, pnY, pnWidth, pnHeight);
}

bool BaseSystem::AttachToWindow(void* hWnd)
{
	// The OpenXR runtime shows the frames on the headset itself: there's no window to use, and nothing for the game to
	// do differently, so this succeeds
	return true;
}

bool BaseSystem::LoadRenderModel(const char* pchRenderModelName, vr::IVRRenderModels_001::RenderModel_t* pRenderModel)
{
	return GetCreateBaseRenderModels()->LoadRenderModelInline(pchRenderModelName, pRenderModel);
}

void BaseSystem::FreeRenderModel(vr::IVRRenderModels_001::RenderModel_t* pRenderModel)
{
	GetCreateBaseRenderModels()->FreeRenderModelInline(pRenderModel);
}

bool BaseSystem::HandleControllerOverlayInteractionAsMouse(const vr::Compositor_OverlaySettings& overlaySettings,
    HmdVector2_t vecWindowClientPositionOnScreen, HmdVector2_t vecWindowClientSize,
    TrackedDeviceIndex_t unControllerDeviceIndex, EVRControllerEventOutputType eOutputType)
{
	// Not pointed at the overlay: no mouse event sent
	OC_UNSUPPORTED(false, "HandleControllerOverlayInteractionAsMouse (IVRSystem_003): no mouse events from compositor overlays");
}

bool BaseSystem::PollNextEventWithPoseAndOverlays(ETrackingUniverseOrigin eOrigin, VREvent_t* pEvent, uint32_t uncbVREvent,
    TrackedDevicePose_t* pTrackedDevicePose, VROverlayHandle_t* pulOverlayHandle)
{
	if (pulOverlayHandle)
		*pulOverlayHandle = k_ulOverlayHandleInvalid;
	return PollNextEventWithPose(eOrigin, pEvent, uncbVREvent, pTrackedDevicePose);
}

// The eye gaze in the head's frame for the frame being rendered (its -Z the direction looked in): false without one.
// Call with the session locked (shared).
static bool GazeInHead(glm::quat* gazeInHead)
{
	BaseInput* input = GetUnsafeBaseInput();
	XrPosef gaze;
	if (!input || !xr_gbl || !input->GetEyeGaze(xr_gbl->viewSpace, xr_gbl->GetBestTime(), &gaze)) {
		OOVR_LOG_ONCE("Eye-tracked foveation: no eye gaze (eyeTracking off, no eye tracker, no valid gaze yet, or the game's input isn't loaded yet), the game keeps its own foveation centre");
		return false;
	}
	OOVR_LOG_ONCE("Eye-tracked foveation: the game gets the eye gaze");
	*gazeInHead = X2G_quat(gaze.orientation);
	return true;
}

// An eye's orientation in the head's frame, which a headset with canted displays turns outwards. Call with the
// session locked (shared).
static glm::quat EyeInHead(EVREye eye)
{
	if (!xr_gbl)
		return glm::identity<glm::quat>();
	XruCachedViews cached = xr_gbl->GetCachedViews(xr_gbl->viewSpace);
	if (cached.viewCount != XruEyeCount || !(cached.viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
		return glm::identity<glm::quat>();
	return X2G_quat(cached.views[eye].pose.orientation);
}

bool BaseSystem::GetEyeTrackedFoveationCenter(HmdVector2_t* pNdcLeft, HmdVector2_t* pNdcRight)
{
	// Zeroes and false without a gaze
	if (pNdcLeft)
		*pNdcLeft = {};
	if (pNdcRight)
		*pNdcRight = {};
	if (!pNdcLeft || !pNdcRight)
		return false;

	auto lock = xr_session.lock_shared();
	glm::quat gaze;
	if (!GazeInHead(&gaze))
		return false;

	// Through each eye's own projection, as GetProjectionMatrix gives it (the depth range doesn't matter)
	std::optional<HmdVector2_t> centres[2];
	for (EVREye eye : { Eye_Left, Eye_Right }) {
		glm::vec3 direction = Foveation::GazeInEye(gaze, EyeInHead(eye));
		centres[eye] = Foveation::CenterForProjection(direction, GetProjectionMatrix(eye, 0.1f, 100.0f));
	}
	if (!centres[Eye_Left] || !centres[Eye_Right])
		return false;
	*pNdcLeft = *centres[Eye_Left];
	*pNdcRight = *centres[Eye_Right];
	return true;
}

bool BaseSystem::GetEyeTrackedFoveationCenterForProjection(const HmdMatrix44_t* pProjMat, HmdVector2_t* pNdc)
{
	if (pNdc)
		*pNdc = {};
	if (!pProjMat || !pNdc)
		return false;

	auto lock = xr_session.lock_shared();
	glm::quat gaze;
	if (!GazeInHead(&gaze))
		return false;

	// The matrix doesn't say which eye it is for; that matters only where the eyes are turned. The eye whose
	// projection it is nearer to, else the head.
	int eye = Foveation::EyeOfProjection(*pProjMat, GetProjectionMatrix(Eye_Left, 0.1f, 100.0f), GetProjectionMatrix(Eye_Right, 0.1f, 100.0f));
	glm::quat eyeInHead = eye < 0 ? glm::identity<glm::quat>() : EyeInHead((EVREye)eye);

	std::optional<HmdVector2_t> centre = Foveation::CenterForProjection(Foveation::GazeInEye(gaze, eyeInHead), *pProjMat);
	if (!centre)
		return false;
	*pNdc = *centre;
	return true;
}

EVRInitError BaseSystem::SetSDKVersion(uint32_t nVersionMajor, uint32_t nVersionMinor, uint32_t nVersionBuild)
{
	OOVR_LOGF("App built with OpenVR SDK %u.%u.%u", nVersionMajor, nVersionMinor, nVersionBuild);
	return VRInitError_None;
}
