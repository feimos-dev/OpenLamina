//
// Created by ZNix on 25/10/2020.
//

#pragma once

// TODO Turtle1331 it would be nicer to include OpenXR-SDK's xr_dependencies.h if possible
#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
#include <d3d11.h>
#endif

#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)
#include <d3d12.h>
#endif

#ifdef SUPPORT_GL
#include <GL/gl.h>

#ifndef _WIN32
// Currently just support XLIB
#include <GL/glx.h>
#endif

#endif

#ifdef SUPPORT_GLES
#include <EGL/egl.h>
#endif

#ifdef SUPPORT_VK
#include <vulkan/vulkan.h>
#endif

#ifdef ANDROID
#include <jni.h>
#endif

#include "../logging.h"

#include "../RuntimeExtensions/XR_MNDX_xdev_space.h"
#include <openxr/openxr_platform.h>

#include <set>
#include <string>
#include <vector>

typedef uint32_t XrGraphicsApiSupportedFlags;

// Extensions that only add an interaction profile, newer than the bundled OpenXR headers
#define OC_XR_HTC_VIVE_COSMOS_CONTROLLER_INTERACTION_EXTENSION_NAME "XR_HTC_vive_cosmos_controller_interaction"
#define OC_XR_HTC_VIVE_FOCUS3_CONTROLLER_INTERACTION_EXTENSION_NAME "XR_HTC_vive_focus3_controller_interaction"
#define OC_XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME "XR_BD_controller_interaction"

// XR_FB_display_refresh_rate, which the bundled OpenXR headers predate (named apart from the newer headers' own)
#define OC_XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME "XR_FB_display_refresh_rate"
static const XrStructureType OC_XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB = (XrStructureType)1000101000;
struct OcXrEventDataDisplayRefreshRateChangedFB {
	XrStructureType type;
	const void* next;
	float fromDisplayRefreshRate;
	float toDisplayRefreshRate;
};
typedef XrResult(XRAPI_PTR* OcPFN_xrGetDisplayRefreshRateFB)(XrSession session, float* displayRefreshRate);
typedef XrResult(XRAPI_PTR* OcPFN_xrEnumerateDisplayRefreshRatesFB)(XrSession session, uint32_t capacity, uint32_t* count, float* rates);
typedef XrResult(XRAPI_PTR* OcPFN_xrRequestDisplayRefreshRateFB)(XrSession session, float displayRefreshRate);
// The OpenXR version xr_instance was made for
extern XrVersion xr_instance_version;

// XR_EXT_local_floor, newer than the bundled OpenXR headers (and core in OpenXR 1.1)
#define OC_XR_EXT_LOCAL_FLOOR_EXTENSION_NAME "XR_EXT_local_floor"
static const XrReferenceSpaceType OC_XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR = (XrReferenceSpaceType)1000426000;
// XR_EXT_user_presence, newer than the bundled OpenXR headers: whether the user wears the headset
#define OC_XR_EXT_USER_PRESENCE_EXTENSION_NAME "XR_EXT_user_presence"
static const XrStructureType OC_XR_TYPE_EVENT_DATA_USER_PRESENCE_CHANGED_EXT = (XrStructureType)1000470000;
struct OcXrEventDataUserPresenceChangedEXT {
	XrStructureType type;
	const void* next;
	XrSession session;
	XrBool32 isUserPresent;
};
static const XrStructureType OC_XR_TYPE_SYSTEM_USER_PRESENCE_PROPERTIES_EXT = (XrStructureType)1000470001;
struct OcXrSystemUserPresencePropertiesEXT {
	XrStructureType type;
	void* next;
	XrBool32 supportsUserPresence;
};

// Flag bits for supported graphics apis
static const XrGraphicsApiSupportedFlags XR_SUPPORTED_GRAPHICS_API_D3D11 = 0x0001;
static const XrGraphicsApiSupportedFlags XR_SUPPORTED_GRAPHICS_API_D3D12 = 0x0002;
static const XrGraphicsApiSupportedFlags XR_SUPPORTED_GRAPHICS_API_GL = 0x0004;
static const XrGraphicsApiSupportedFlags XR_SUPPORTED_GRAPHICS_API_GLES = 0x0008;
static const XrGraphicsApiSupportedFlags XR_SUPPORTED_GRAPHICS_API_VK = 0x0010;

/**
 * A wrapper class for the function pointers to OpenXR extensions.
 *
 * Modern versions of the OpenXR SDK don't provide implementations of the extension
 * methods, and we have to use xrGetInstanceProcAddr to get said function pointers
 * manually. One global instance of XrExt is defined in xrutil.h as xr_ext which is
 * setup when the OpenXR instance is created.
 *
 * See https://github.com/microsoft/OpenXR-MixedReality/issues/32
 */
class XrExt {
public:
	XrExt(XrGraphicsApiSupportedFlags apis, const std::vector<const char*>& extensions);

	bool G2Controller_Available() { return supportsG2Controller; }
	// Whether the instance was created with that extension
	bool ExtensionEnabled(const std::string& name) { return enabledExtensions.contains(name); }
	bool CylinderKHR_Available() { return supportsCylinderKHR; }
	bool EyeGaze_Available() { return supportsEyeGaze; }
	bool DepthKHR_Available() { return supportsDepthKHR; }
	bool Equirect2KHR_Available() { return supportsEquirect2KHR; }
	bool ColorScaleBiasKHR_Available() { return supportsColorScaleBiasKHR; }
	bool UserPresence_Available() { return supportsUserPresence; }
	// LOCAL_FLOOR reference spaces: XR_EXT_local_floor, or OpenXR 1.1's core
	bool LocalFloor_Available() { return supportsLocalFloor || xr_instance_version >= XR_MAKE_VERSION(1, 1, 0); }
	bool xrGetVisibilityMaskKHR_Available() { return pfnXrGetVisibilityMaskKHR != nullptr; }
	bool xrMndxXdevSpace_Available()
	{
		return pfnxrCreateXDevListMNDX && pfnxrGetXDevListGenerationNumberMNDX && pfnxrEnumerateXDevsMNDX
		    && pfnxrGetXDevPropertiesMNDX && pfnxrDestroyXDevListMNDX && pfnxrCreateXDevSpaceMNDX;
	}

	XrResult xrGetVisibilityMaskKHR(
	    XrSession session,
	    XrViewConfigurationType viewConfigurationType,
	    uint32_t viewIndex,
	    XrVisibilityMaskTypeKHR visibilityMaskType,
	    XrVisibilityMaskKHR* visibilityMask)
	{
		if (!pfnXrGetVisibilityMaskKHR)
			return XR_ERROR_FUNCTION_UNSUPPORTED; // the runtime lacks the extension; callers check the result
		return pfnXrGetVisibilityMaskKHR(session, viewConfigurationType, viewIndex, visibilityMaskType, visibilityMask);
	}

	bool DisplayRefreshRateFB_Available() { return pfnXrGetDisplayRefreshRateFB != nullptr; }
	XrResult xrGetDisplayRefreshRateFB(XrSession session, float* displayRefreshRate)
	{
		if (!pfnXrGetDisplayRefreshRateFB)
			return XR_ERROR_FUNCTION_UNSUPPORTED; // the runtime lacks the extension; callers check the result
		return pfnXrGetDisplayRefreshRateFB(session, displayRefreshRate);
	}

	XrResult xrEnumerateDisplayRefreshRatesFB(XrSession session, uint32_t capacity, uint32_t* count, float* rates)
	{
		if (!pfnXrEnumerateDisplayRefreshRatesFB)
			return XR_ERROR_FUNCTION_UNSUPPORTED;
		return pfnXrEnumerateDisplayRefreshRatesFB(session, capacity, count, rates);
	}
	bool RequestDisplayRefreshRateFB_Available() { return pfnXrRequestDisplayRefreshRateFB != nullptr; }
	XrResult xrRequestDisplayRefreshRateFB(XrSession session, float displayRefreshRate)
	{
		if (!pfnXrRequestDisplayRefreshRateFB)
			return XR_ERROR_FUNCTION_UNSUPPORTED;
		return pfnXrRequestDisplayRefreshRateFB(session, displayRefreshRate);
	}

	bool handTrackingExtensionAvailable() { return pfnXrCreateHandTrackerExt != nullptr; }
	XrResult xrCreateHandTrackerEXT(XrSession session, const XrHandTrackerCreateInfoEXT* createInfo, XrHandTrackerEXT* handTracker)
	{
		if (!pfnXrCreateHandTrackerExt)
			return XR_ERROR_FUNCTION_UNSUPPORTED; // the runtime lacks the extension; callers check the result
		return pfnXrCreateHandTrackerExt(session, createInfo, handTracker);
	}
	XrResult xrDestroyHandTrackerEXT(XrHandTrackerEXT handTracker)
	{
		if (!pfnXrDestroyHandTrackerExt)
			return XR_ERROR_FUNCTION_UNSUPPORTED; // the runtime lacks the extension; callers check the result
		return pfnXrDestroyHandTrackerExt(handTracker);
	}
	XrResult xrLocateHandJointsEXT(XrHandTrackerEXT handTracker, const XrHandJointsLocateInfoEXT* locateInfo, XrHandJointLocationsEXT* locations)
	{
		if (!pfnXrLocateHandJointsExt)
			return XR_ERROR_FUNCTION_UNSUPPORTED; // the runtime lacks the extension; callers check the result
		return pfnXrLocateHandJointsExt(handTracker, locateInfo, locations);
	}

	XrResult xrCreateXDevListMNDX(XrSession session, const XrCreateXDevListInfoMNDX* createInfo, XrXDevListMNDX* xdevList)
	{
		if (!pfnxrCreateXDevListMNDX)
			return XR_ERROR_FUNCTION_UNSUPPORTED; // the runtime lacks the extension; callers check the result
		return pfnxrCreateXDevListMNDX(session, createInfo, xdevList);
	}

	XrResult xrGetXDevListGenerationNumberMNDX(XrXDevListMNDX xdevList, uint64_t* outGeneration)
	{
		if (!pfnxrGetXDevListGenerationNumberMNDX)
			return XR_ERROR_FUNCTION_UNSUPPORTED; // the runtime lacks the extension; callers check the result
		return pfnxrGetXDevListGenerationNumberMNDX(xdevList, outGeneration);
	}

	XrResult xrEnumerateXDevsMNDX(XrXDevListMNDX xdevList, uint32_t xdevCapacityInput, uint32_t* xdevCountOutput, XrXDevIdMNDX* xdevs)
	{
		if (!pfnxrEnumerateXDevsMNDX)
			return XR_ERROR_FUNCTION_UNSUPPORTED; // the runtime lacks the extension; callers check the result
		return pfnxrEnumerateXDevsMNDX(xdevList, xdevCapacityInput, xdevCountOutput, xdevs);
	}

	XrResult xrGetXDevPropertiesMNDX(XrXDevListMNDX xdevList, const XrGetXDevInfoMNDX* info, XrXDevPropertiesMNDX* properties)
	{
		if (!pfnxrGetXDevPropertiesMNDX)
			return XR_ERROR_FUNCTION_UNSUPPORTED; // the runtime lacks the extension; callers check the result
		return pfnxrGetXDevPropertiesMNDX(xdevList, info, properties);
	}

	XrResult xrDestroyXDevListMNDX(XrXDevListMNDX xdevList)
	{
		if (!pfnxrDestroyXDevListMNDX)
			return XR_ERROR_FUNCTION_UNSUPPORTED; // the runtime lacks the extension; callers check the result
		return pfnxrDestroyXDevListMNDX(xdevList);
	}

	XrResult xrCreateXDevSpaceMNDX(XrSession session, const XrCreateXDevSpaceInfoMNDX* createInfo, XrSpace* space)
	{
		if (!pfnxrCreateXDevSpaceMNDX)
			return XR_ERROR_FUNCTION_UNSUPPORTED; // the runtime lacks the extension; callers check the result
		return pfnxrCreateXDevSpaceMNDX(session, createInfo, space);
	}

#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	bool xrGetD3D11GraphicsRequirementsKHR_Available()
	{
		return pfnXrGetD3D11GraphicsRequirementsKHR != nullptr;
	}
	XrResult xrGetD3D11GraphicsRequirementsKHR(XrInstance instance, XrSystemId systemId, XrGraphicsRequirementsD3D11KHR* graphicsRequirements)
	{
		if (!pfnXrGetD3D11GraphicsRequirementsKHR)
			return XR_ERROR_FUNCTION_UNSUPPORTED; // the runtime lacks the extension; callers check the result
		return pfnXrGetD3D11GraphicsRequirementsKHR(instance, systemId, graphicsRequirements);
	}
#endif
#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)
	bool xrGetD3D12GraphicsRequirementsKHR_Available()
	{
		return pfnXrGetD3D12GraphicsRequirementsKHR != nullptr;
	}
	XrResult xrGetD3D12GraphicsRequirementsKHR(XrInstance instance, XrSystemId systemId, XrGraphicsRequirementsD3D12KHR* graphicsRequirements)
	{
		if (!pfnXrGetD3D12GraphicsRequirementsKHR)
			return XR_ERROR_FUNCTION_UNSUPPORTED; // the runtime lacks the extension; callers check the result
		return pfnXrGetD3D12GraphicsRequirementsKHR(instance, systemId, graphicsRequirements);
	}
#endif
#ifdef SUPPORT_VK
	bool xrGetVulkanInstanceExtensionsKHR_Available()
	{
		return pfnXrGetVulkanInstanceExtensionsKHR != nullptr;
	}
	XrResult xrGetVulkanInstanceExtensionsKHR(XrInstance instance, XrSystemId systemId, uint32_t bufferCapacityInput, uint32_t* bufferCountOutput, char* buffer)
	{
		if (!pfnXrGetVulkanInstanceExtensionsKHR)
			return XR_ERROR_FUNCTION_UNSUPPORTED; // the runtime lacks the extension; callers check the result
		return pfnXrGetVulkanInstanceExtensionsKHR(instance, systemId, bufferCapacityInput, bufferCountOutput, buffer);
	}
	bool xrGetVulkanDeviceExtensionsKHR_Available() { return pfnXrGetVulkanDeviceExtensionsKHR != nullptr; }
	XrResult xrGetVulkanDeviceExtensionsKHR(XrInstance instance, XrSystemId systemId, uint32_t bufferCapacityInput, uint32_t* bufferCountOutput, char* buffer)
	{
		if (!pfnXrGetVulkanDeviceExtensionsKHR)
			return XR_ERROR_FUNCTION_UNSUPPORTED; // the runtime lacks the extension; callers check the result
		return pfnXrGetVulkanDeviceExtensionsKHR(instance, systemId, bufferCapacityInput, bufferCountOutput, buffer);
	}
	bool xrGetVulkanGraphicsDeviceKHR_Available() { return pfnXrGetVulkanGraphicsDeviceKHR != nullptr; }
	XrResult xrGetVulkanGraphicsDeviceKHR(XrInstance instance, XrSystemId systemId, VkInstance vkInstance, VkPhysicalDevice* vkPhysicalDevice)
	{
		if (!pfnXrGetVulkanGraphicsDeviceKHR)
			return XR_ERROR_FUNCTION_UNSUPPORTED; // the runtime lacks the extension; callers check the result
		return pfnXrGetVulkanGraphicsDeviceKHR(instance, systemId, vkInstance, vkPhysicalDevice);
	}
	bool xrGetVulkanGraphicsRequirementsKHR_Available() { return pfnXrGetVulkanGraphicsRequirementsKHR != nullptr; }
	XrResult xrGetVulkanGraphicsRequirementsKHR(XrInstance instance, XrSystemId systemId, XrGraphicsRequirementsVulkanKHR* graphicsRequirements)
	{
		if (!pfnXrGetVulkanGraphicsRequirementsKHR)
			return XR_ERROR_FUNCTION_UNSUPPORTED; // the runtime lacks the extension; callers check the result
		return pfnXrGetVulkanGraphicsRequirementsKHR(instance, systemId, graphicsRequirements);
	}
#endif
#ifdef SUPPORT_GL
	bool xrGetOpenGLGraphicsRequirementsKHR_Available()
	{
		return pfnXrGetOpenGLGraphicsRequirementsKHR != nullptr;
	}
	XrResult xrGetOpenGLGraphicsRequirementsKHR(XrInstance instance, XrSystemId systemId, XrGraphicsRequirementsOpenGLKHR* graphicsRequirements)
	{
		if (!pfnXrGetOpenGLGraphicsRequirementsKHR)
			return XR_ERROR_FUNCTION_UNSUPPORTED; // the runtime lacks the extension; callers check the result
		return pfnXrGetOpenGLGraphicsRequirementsKHR(instance, systemId, graphicsRequirements);
	}
#endif
#ifdef SUPPORT_GLES
	bool xrGetOpenGLESGraphicsRequirementsKHR_Available()
	{
		return pfnXrGetOpenGLESGraphicsRequirementsKHR != nullptr;
	}
	XrResult xrGetOpenGLESGraphicsRequirementsKHR(XrInstance instance, XrSystemId systemId, XrGraphicsRequirementsOpenGLESKHR* graphicsRequirements)
	{
		if (!pfnXrGetOpenGLESGraphicsRequirementsKHR)
			return XR_ERROR_FUNCTION_UNSUPPORTED; // the runtime lacks the extension; callers check the result
		return pfnXrGetOpenGLESGraphicsRequirementsKHR(instance, systemId, graphicsRequirements);
	}
#endif

private:
	PFN_xrGetVisibilityMaskKHR pfnXrGetVisibilityMaskKHR = nullptr;
	PFN_xrCreateHandTrackerEXT pfnXrCreateHandTrackerExt = nullptr;
	PFN_xrDestroyHandTrackerEXT pfnXrDestroyHandTrackerExt = nullptr;
	PFN_xrLocateHandJointsEXT pfnXrLocateHandJointsExt = nullptr;

	PFN_xrCreateXDevListMNDX pfnxrCreateXDevListMNDX = nullptr;
	PFN_xrGetXDevListGenerationNumberMNDX pfnxrGetXDevListGenerationNumberMNDX = nullptr;
	PFN_xrEnumerateXDevsMNDX pfnxrEnumerateXDevsMNDX = nullptr;
	PFN_xrGetXDevPropertiesMNDX pfnxrGetXDevPropertiesMNDX = nullptr;
	PFN_xrDestroyXDevListMNDX pfnxrDestroyXDevListMNDX = nullptr;
	PFN_xrCreateXDevSpaceMNDX pfnxrCreateXDevSpaceMNDX = nullptr;

	OcPFN_xrGetDisplayRefreshRateFB pfnXrGetDisplayRefreshRateFB = nullptr;
	OcPFN_xrEnumerateDisplayRefreshRatesFB pfnXrEnumerateDisplayRefreshRatesFB = nullptr;
	OcPFN_xrRequestDisplayRefreshRateFB pfnXrRequestDisplayRefreshRateFB = nullptr;

	std::set<std::string> enabledExtensions;
	bool supportsG2Controller = false;
	bool supportsCylinderKHR = false;
	bool supportsEyeGaze = false;
	bool supportsDepthKHR = false;
	bool supportsEquirect2KHR = false;
	bool supportsColorScaleBiasKHR = false;
	bool supportsUserPresence = false;
	bool supportsLocalFloor = false;

#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	PFN_xrGetD3D11GraphicsRequirementsKHR pfnXrGetD3D11GraphicsRequirementsKHR = nullptr;
#endif
#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)
	PFN_xrGetD3D12GraphicsRequirementsKHR pfnXrGetD3D12GraphicsRequirementsKHR = nullptr;
#endif
#ifdef SUPPORT_VK
	PFN_xrGetVulkanGraphicsRequirementsKHR pfnXrGetVulkanGraphicsRequirementsKHR = nullptr;
	PFN_xrGetVulkanInstanceExtensionsKHR pfnXrGetVulkanInstanceExtensionsKHR = nullptr;
	PFN_xrGetVulkanDeviceExtensionsKHR pfnXrGetVulkanDeviceExtensionsKHR = nullptr;
	PFN_xrGetVulkanGraphicsDeviceKHR pfnXrGetVulkanGraphicsDeviceKHR = nullptr;
#endif
#ifdef SUPPORT_GL
	PFN_xrGetOpenGLGraphicsRequirementsKHR pfnXrGetOpenGLGraphicsRequirementsKHR = nullptr;
#endif
#ifdef SUPPORT_GLES
	PFN_xrGetOpenGLESGraphicsRequirementsKHR pfnXrGetOpenGLESGraphicsRequirementsKHR = nullptr;
#endif
};

// Put this here rather tha xrutil.h so not all files have to include vector
extern std::vector<XrViewConfigurationView> xr_views_list;
