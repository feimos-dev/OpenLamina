//
// Created by ZNix on 25/10/2020.
//

#include "DrvOpenXR.h"

#include "../OpenOVR/Misc/Config.h"
#include "../OpenOVR/Misc/Process.h"
#include "../OpenOVR/Misc/SharedStore.h"
#include "../OpenOVR/Misc/android_api.h"
#include "../OpenOVR/Misc/xr_ext.h"
#include "../OpenOVR/Reimpl/BaseInput.h"
#include "../OpenOVR/Reimpl/BaseSystem.h"
#include "../RuntimeExtensions/XR_MNDX_xdev_space.h"
#include "HelperSession.h"
#include "XrBackend.h"
#include "XrGenericTracker.h"
#include "generated/static_bases.gen.h"
#include "json/json.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

static XrBackend* currentBackend;
static bool initialised = false;
// The overlay session's create info, chained into xrCreateSession
static HelperSessionChain helperChain;

#ifdef _WIN32
std::string GetExeName()
{
	char exePath[MAX_PATH + 1] = { 0 };
	DWORD len = GetModuleFileNameA(NULL, exePath, MAX_PATH);
	PathStripPathA(exePath);
	return { exePath };
}
#else
#include <libgen.h> // basename
#include <linux/limits.h> // PATH_MAX
#include <unistd.h> // readlink

std::string GetExeName()
{
	char exePath[PATH_MAX + 1] = { 0 };
	ssize_t count = readlink("/proc/self/exe", exePath, PATH_MAX);
	if (count != -1) {
		return { basename(exePath) };
	}
	return { "" };
}
#endif

void DrvOpenXR::GetXRAppName(char (&appName)[128])
{
	std::string exeName = GetExeName();
	if (exeName.size() > 0) {
		std::string ocAppName{ "OpenLamina_" };
		ocAppName.append(exeName);
		auto pos = ocAppName.find(".exe");
		if (pos != std::string::npos && pos > 0)
			ocAppName = ocAppName.substr(0, pos);
		OOVR_LOGF("Setting application name to %s", ocAppName.c_str());
		strcpy_arr(appName, ocAppName.c_str());
	} else {
		strcpy_arr(appName, "OpenLamina");
	}
}

#ifdef _DEBUG
static XrDebugUtilsMessengerEXT dbgMessenger = NULL;

static XrBool32 XRAPI_CALL debugCallback(
    XrDebugUtilsMessageSeverityFlagsEXT messageSeverity,
    XrDebugUtilsMessageTypeFlagsEXT messageTypes,
    const XrDebugUtilsMessengerCallbackDataEXT* callbackData,
    void* userData)
{
	OOVR_LOGF("debugCallback %s", callbackData->message);
	return XR_FALSE;
}
#endif

static XrResult CreateSystemID()
{
	// Create a system - this is when we choose what form factor we want, in this case an HMD
	XrSystemGetInfo systemInfo{};
	systemInfo.type = XR_TYPE_SYSTEM_GET_INFO;
	systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrResult res = xrGetSystem(xr_instance, &systemInfo, &xr_system);
	if (XR_FAILED(res))
		OOVR_LOGF("xrGetSystem failed: %d (no headset?)", (int)res);
	return res;
}

// The init error an OpenVR game gets for a start-up call that failed
static vr::EVRInitError InitErrorFor(XrResult res)
{
	// -51 is XR_ERROR_RUNTIME_UNAVAILABLE (OpenXR 1.0.29), newer than the bundled headers (so not a case: gcc's
	// -Wswitch rejects a value outside the enum)
	if ((int)res == -51 || res == XR_ERROR_FORM_FACTOR_UNAVAILABLE || res == XR_ERROR_FORM_FACTOR_UNSUPPORTED)
		return vr::VRInitError_Init_HmdNotFound;
	return vr::VRInitError_Init_Internal;
}

// Destroys what a failed CreateOpenXRBackend had created, and answers nullptr with the error
static IBackend* StartupFailed(vr::EVRInitError* error, vr::EVRInitError e)
{
	if (currentBackend) {
		// Its destructor shuts the session and the instance down (FullShutdown)
		delete currentBackend;
	} else {
#ifdef _DEBUG
		// Destroyed with the instance; the next start-up mustn't destroy it again
		if (dbgMessenger != NULL)
			xrDestroyDebugUtilsMessengerEXT(dbgMessenger);
		dbgMessenger = NULL;
#endif
		if (xr_instance)
			xrDestroyInstance(xr_instance);
		xr_instance = XR_NULL_HANDLE;
	}
	// The extension functions, with or without a backend (FullShutdown leaves them)
	delete xr_ext;
	xr_ext = nullptr;
	currentBackend = nullptr;
	initialised = false;
	SetCurrentSessionKind(SessionKind::Scene);
	if (error)
		*error = e;
	return nullptr;
}

static void handleStartupInfo(const char* startupInfo)
{
	// According to some Proton code (vrclient_x64/json_converter.cpp::json_convert_startup_info), the startup info is json.
	if (!startupInfo) {
		return;
	}

	std::string info(startupInfo);
	OOVR_LOGF("got startup info: %s", startupInfo);

	Json::CharReaderBuilder builder;
	auto reader = std::unique_ptr<Json::CharReader>(builder.newCharReader());
	Json::Value root;
	std::string errs;
	if (!reader->parse(info.data(), info.data() + info.size(), &root, &errs)) {
		OOVR_LOGF("WARNING: Failed to parse startupInfo: %s (%c)", errs.c_str(), errs.back());
		return;
	}

	if (!root.isMember("action_manifest_path")) {
		return;
	}

	std::string manifest = root["action_manifest_path"].asString();
	OOVR_LOGF("Got action manifest path: %s", manifest.c_str());
	BaseInput::setStartupManifest(manifest);
}

namespace {

// The runtime's instance extensions
XrResult AvailableInstanceExtensions(std::set<std::string>& available)
{
	uint32_t count = 0;
	if (XrResult res = xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr); XR_FAILED(res)) {
		OOVR_LOGF("Failed to enumerate XR extensions: %d", res);
		return res;
	}
	std::vector<XrExtensionProperties> properties(count, { XR_TYPE_EXTENSION_PROPERTIES });
	if (XrResult res = xrEnumerateInstanceExtensionProperties(nullptr, (uint32_t)properties.size(), &count, properties.data()); XR_FAILED(res)) {
		OOVR_LOGF("Failed to enumerate XR extensions: %d", res);
		return res;
	}
	properties.resize(count);
	for (const XrExtensionProperties& ext : properties)
		available.insert(ext.extensionName);
	return XR_SUCCESS;
}

// What an instance is made with
struct InstancePlan {
	std::vector<const char*> extensions;
	XrGraphicsApiSupportedFlags apiFlags = 0;
};

// The extensions OpenLamina enables, of those available, for a session of that kind
InstancePlan PlanInstance(const std::set<std::string>& available, SessionKind kind)
{
	InstancePlan plan;

#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	if (available.count("XR_KHR_D3D11_enable")) {
		plan.extensions.push_back("XR_KHR_D3D11_enable");
		plan.apiFlags |= XR_SUPPORTED_GRAPHICS_API_D3D11;
	}
#endif
#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)
	if (available.count("XR_KHR_D3D12_enable")) {
		plan.extensions.push_back("XR_KHR_D3D12_enable");
		plan.apiFlags |= XR_SUPPORTED_GRAPHICS_API_D3D12;
	}
#endif
#if defined(SUPPORT_VK)
	if (available.count(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME)) {
		plan.extensions.push_back(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
		plan.apiFlags |= XR_SUPPORTED_GRAPHICS_API_VK;
	}
#endif
#if defined(SUPPORT_GL)
	if (available.count(XR_KHR_OPENGL_ENABLE_EXTENSION_NAME)) {
		plan.extensions.push_back(XR_KHR_OPENGL_ENABLE_EXTENSION_NAME);
		plan.apiFlags |= XR_SUPPORTED_GRAPHICS_API_GL;
	}
#endif
#if defined(SUPPORT_GLES)
	if (available.count(XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME)) {
		plan.extensions.push_back(XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME);
		plan.apiFlags |= XR_SUPPORTED_GRAPHICS_API_GLES;
	}
#endif
#if defined(ANDROID)
	if (available.count(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME))
		plan.extensions.push_back(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
#endif
	if (available.count(XR_EXT_DEBUG_UTILS_EXTENSION_NAME))
		plan.extensions.push_back(XR_EXT_DEBUG_UTILS_EXTENSION_NAME);

	// If the visibility mask is available use it, otherwise no big deal
	if (available.count(XR_KHR_VISIBILITY_MASK_EXTENSION_NAME))
		plan.extensions.push_back(XR_KHR_VISIBILITY_MASK_EXTENSION_NAME);

	if (available.count(XR_EXT_HAND_TRACKING_EXTENSION_NAME)) {
		plan.extensions.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
		// Whether a hand's joints are the hand's own or inferred from a controller
		if (available.contains(OC_XR_EXT_HAND_TRACKING_DATA_SOURCE_EXTENSION_NAME))
			plan.extensions.push_back(OC_XR_EXT_HAND_TRACKING_DATA_SOURCE_EXTENSION_NAME);
	}

	// Vive trackers by their role (/user/vive_tracker_htcx/role/waist), poses only
	if (available.contains(OC_XR_HTCX_VIVE_TRACKER_INTERACTION_EXTENSION_NAME))
		plan.extensions.push_back(OC_XR_HTCX_VIVE_TRACKER_INTERACTION_EXTENSION_NAME);

	if (available.count(XR_MNDX_XDEV_SPACE_EXTENSION_NAME))
		plan.extensions.push_back(XR_MNDX_XDEV_SPACE_EXTENSION_NAME);

	if (available.contains(XR_EXT_HP_MIXED_REALITY_CONTROLLER_EXTENSION_NAME))
		plan.extensions.push_back(XR_EXT_HP_MIXED_REALITY_CONTROLLER_EXTENSION_NAME);

	if (available.contains(OC_XR_HTC_VIVE_COSMOS_CONTROLLER_INTERACTION_EXTENSION_NAME))
		plan.extensions.push_back(OC_XR_HTC_VIVE_COSMOS_CONTROLLER_INTERACTION_EXTENSION_NAME);

	if (available.contains(OC_XR_HTC_VIVE_FOCUS3_CONTROLLER_INTERACTION_EXTENSION_NAME))
		plan.extensions.push_back(OC_XR_HTC_VIVE_FOCUS3_CONTROLLER_INTERACTION_EXTENSION_NAME);

	if (available.contains(OC_XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME))
		plan.extensions.push_back(OC_XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME);

	// More controllers' profiles: the Quest Pro's and Quest 3's, the Touch's finger proximity, Samsung's Odyssey, and
	// the hands as controllers
	for (const char* profileExtension : { OC_XR_FB_TOUCH_CONTROLLER_PRO_EXTENSION_NAME, OC_XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME,
	         OC_XR_FB_TOUCH_CONTROLLER_PROXIMITY_EXTENSION_NAME, XR_EXT_SAMSUNG_ODYSSEY_CONTROLLER_EXTENSION_NAME, XR_MSFT_HAND_INTERACTION_EXTENSION_NAME,
	         OC_XR_EXT_HAND_INTERACTION_EXTENSION_NAME })
		if (available.contains(profileExtension))
			plan.extensions.push_back(profileExtension);

	if (available.contains(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME))
		plan.extensions.push_back(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);

	// The gaze for eye-tracked foveation; a runtime may start its eye tracker for it, so only when the user wants it
	if (oovr_global_configuration.EyeTracking() && available.contains(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME))
		plan.extensions.push_back(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
	// Depth with the projection layer (submitDepth), equirect layers for skyboxes, and overlays' colour and alpha, where
	// the runtime has them
	for (const char* layerExtension : { XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME, XR_KHR_COMPOSITION_LAYER_EQUIRECT2_EXTENSION_NAME, XR_KHR_COMPOSITION_LAYER_COLOR_SCALE_BIAS_EXTENSION_NAME })
		if (available.contains(layerExtension))
			plan.extensions.push_back(layerExtension);
	// The display's refresh rate (Prop_DisplayFrequency_Float) and its changes
	if (available.contains(OC_XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME))
		plan.extensions.push_back(OC_XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
	// Notices that the runtime's CPU or GPU load is high (ShouldApplicationReduceRenderingWork)
	if (available.contains(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME))
		plan.extensions.push_back(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
	// A helper program's session
	if (kind == SessionKind::Overlay) {
		plan.extensions.push_back(XR_EXTX_OVERLAY_EXTENSION_NAME);
	} else if (kind == SessionKind::Headless) {
		plan.extensions.push_back(XR_MND_HEADLESS_EXTENSION_NAME);
	}
	// The time now, which the runtime converts from the system's clock: the latest tracking data, poses predicted from
	// now, the time since the last vsync, and a headless session's poses (it runs no frames)
#ifdef _WIN32
	const char* clock = "XR_KHR_win32_convert_performance_counter_time";
#else
	const char* clock = "XR_KHR_convert_timespec_time";
#endif
	if (available.contains(clock))
		plan.extensions.push_back(clock);
	else if (kind == SessionKind::Headless)
		OOVR_LOGF("The OpenXR runtime has no %s: a background application's poses are located at its session's last state change", clock);
	// The local space at the floor's height, for standing poses where the runtime has no stage
	if (available.contains(OC_XR_EXT_LOCAL_FLOOR_EXTENSION_NAME))
		plan.extensions.push_back(OC_XR_EXT_LOCAL_FLOOR_EXTENSION_NAME);
	// The headset's proximity sensor
	if (available.contains(OC_XR_EXT_USER_PRESENCE_EXTENSION_NAME))
		plan.extensions.push_back(OC_XR_EXT_USER_PRESENCE_EXTENSION_NAME);

	return plan;
}

// Makes the instance for a plan, at the newest OpenXR version the loader and runtime take
XrResult MakeInstance(const InstancePlan& plan, XrInstance* instance, XrVersion* version)
{
	XrApplicationInfo appInfo{};
	DrvOpenXR::GetXRAppName(appInfo.applicationName);
	appInfo.applicationVersion = 1;
	// Runtimes pick per-engine behaviour by the engine's name
	strcpy_s(appInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "OpenLamina");
	appInfo.engineVersion = 1;

	const char* const layers[] = {
#ifdef XR_VALIDATION_LAYER_PATH
		"XR_APILAYER_LUNARG_core_validation",
#endif
		nullptr // Dummy value since MSVC gets upset if there's nothing in this array
	};

	// The validation layer, where it's built in (TODO make these work on Linux)
#ifdef XR_VALIDATION_LAYER_PATH
	SetEnvironmentVariableA("XR_CORE_VALIDATION_EXPORT_TYPE", "text");
	SetEnvironmentVariableA("XR_API_LAYER_PATH", XR_VALIDATION_LAYER_PATH);
	OOVR_LOGF("Set OpenXR Layer path: %s", XR_VALIDATION_LAYER_PATH);
#endif
#ifdef XR_VALIDATION_FILE_NAME
	SetEnvironmentVariableA("XR_CORE_VALIDATION_FILE_NAME", XR_VALIDATION_FILE_NAME);
	OOVR_LOGF("Set OpenXR validation file path: %s", XR_VALIDATION_FILE_NAME);
#endif

	XrInstanceCreateInfo createInfo{};
	createInfo.type = XR_TYPE_INSTANCE_CREATE_INFO;
	createInfo.applicationInfo = appInfo;
	createInfo.enabledExtensionNames = plan.extensions.data();
	createInfo.enabledExtensionCount = (uint32_t)plan.extensions.size();
	createInfo.enabledApiLayerNames = layers;
	createInfo.enabledApiLayerCount = (sizeof(layers) / sizeof(const char*)) - 1; // Subtract the dummy value

#if ANDROID
	if (!OpenComposite_Android_Create_Info) {
		OOVR_LOG("Cannot create OpenXR instance - OpenComposite_Android_Create_Info not set");
		return XR_ERROR_INITIALIZATION_FAILED;
	}
	XrInstanceCreateInfoAndroidKHR androidInfo = *OpenComposite_Android_Create_Info;
	androidInfo.next = nullptr;
	createInfo.next = &androidInfo;
#endif

	XrResult res = DrvOpenXR::CreateInstanceNewestVersion(createInfo, instance, version, xrCreateInstance);
	if (XR_SUCCEEDED(res))
		OOVR_LOGF("Created the OpenXR instance for OpenXR %d.%d", (int)XR_VERSION_MAJOR(*version), (int)XR_VERSION_MINOR(*version));
	return res;
}

// The instance VR_IsHmdPresent made, with the extensions it was made with, for the scene application's session
std::mutex probeLock;
XrInstance probedInstance = XR_NULL_HANDLE;
XrVersion probedVersion = 0;
std::vector<std::string> probedExtensions;

std::vector<std::string> ExtensionNames(const InstancePlan& plan)
{
	return std::vector<std::string>(plan.extensions.begin(), plan.extensions.end());
}

// The probe's instance where it was made with the plan's extensions; otherwise none, and the probe's is destroyed
XrInstance TakeProbedInstance(const InstancePlan& plan, XrVersion* version)
{
	std::lock_guard<std::mutex> lock(probeLock);
	XrInstance taken = probedInstance;
	*version = probedVersion;
	probedInstance = XR_NULL_HANDLE;
	if (taken && probedExtensions != ExtensionNames(plan)) {
		OOVR_LOG("The OpenXR instance VR_IsHmdPresent made has other extensions: making another");
		xrDestroyInstance(taken);
		taken = XR_NULL_HANDLE;
	} else if (taken) {
		OOVR_LOG("Using the OpenXR instance VR_IsHmdPresent made");
	}
	probedExtensions.clear();
	return taken;
}

} // namespace

XrResult DrvOpenXR::CreateInstanceNewestVersion(XrInstanceCreateInfo createInfo, XrInstance* instance, XrVersion* version,
    XrResult(XRAPI_PTR* create)(const XrInstanceCreateInfo*, XrInstance*))
{
	// OpenXR 1.1 where the loader and runtime have it (its core has what OpenLamina otherwise takes from extensions),
	// else 1.0. A loader or runtime that predates a version answers XR_ERROR_API_VERSION_UNSUPPORTED for it. The
	// headers OpenLamina is built with may be older than either: nothing of 1.1 is used that 1.0's extensions lack.
	static constexpr XrVersion versions[] = { XR_MAKE_VERSION(1, 1, 0), XR_MAKE_VERSION(1, 0, 0) };
	XrResult res = XR_ERROR_API_VERSION_UNSUPPORTED;
	for (XrVersion v : versions) {
		createInfo.applicationInfo.apiVersion = v;
		res = create(&createInfo, instance);
		if (res != XR_ERROR_API_VERSION_UNSUPPORTED) {
			if (XR_SUCCEEDED(res) && version)
				*version = v;
			return res;
		}
		OOVR_LOGF("The OpenXR loader or runtime doesn't support OpenXR %d.%d", (int)XR_VERSION_MAJOR(v), (int)XR_VERSION_MINOR(v));
	}
	return res;
}

bool DrvOpenXR::ProbeForHmd()
{
	std::lock_guard<std::mutex> lock(probeLock);
	XrSystemGetInfo systemInfo{ XR_TYPE_SYSTEM_GET_INFO };
	systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrSystemId id = XR_NULL_SYSTEM_ID;
	// An earlier probe's instance, asked again: the headset may have gone since
	if (probedInstance) {
		if (xrGetSystem(probedInstance, &systemInfo, &id) == XR_SUCCESS)
			return true;
		xrDestroyInstance(probedInstance);
		probedInstance = XR_NULL_HANDLE;
		probedExtensions.clear();
	}

	std::set<std::string> available;
	if (XR_FAILED(AvailableInstanceExtensions(available)))
		return false;
	// As a scene application's: the session that follows takes this instance rather than making a second one
	const InstancePlan plan = PlanInstance(available, SessionKind::Scene);
	XrInstance instance = XR_NULL_HANDLE;
	XrVersion version = 0;
	if (XR_FAILED(MakeInstance(plan, &instance, &version)))
		return false;

	XrResult res = xrGetSystem(instance, &systemInfo, &id);
	if (res == XR_SUCCESS) {
		probedInstance = instance;
		probedVersion = version;
		probedExtensions = ExtensionNames(plan);
		return true;
	}
	xrDestroyInstance(instance);

	// FORM_FACTOR_UNAVAILABLE (no headset plugged in) and anything else: no headset to start (upstream #467)
	if (res != XR_ERROR_FORM_FACTOR_UNAVAILABLE && res != XR_ERROR_FORM_FACTOR_UNSUPPORTED)
		OOVR_LOGF("Failed to probe for OpenXR systems: return status %d", res);
	return false;
}

void DrvOpenXR::ReleaseProbedInstance()
{
	std::lock_guard<std::mutex> lock(probeLock);
	if (probedInstance)
		xrDestroyInstance(probedInstance);
	probedInstance = XR_NULL_HANDLE;
	probedExtensions.clear();
}

IBackend* DrvOpenXR::CreateOpenXRBackend(const char* startupInfo, vr::EVRInitError* error, vr::EVRApplicationType appType)
{
	if (error)
		*error = vr::VRInitError_None;

	// TODO handle something like Unity which stops and restarts the instance
	if (initialised) {
		OOVR_LOG("CreateOpenXRBackend while OpenXR is already running: refused");
		if (error)
			*error = vr::VRInitError_Init_Internal;
		return nullptr;
	}
	initialised = true;

	// The runtime's extensions
	std::set<std::string> availableExtensions;
	if (XrResult res = AvailableInstanceExtensions(availableExtensions); XR_FAILED(res))
		return StartupFailed(error, RefusalWithoutSession(appType, oovr_global_configuration.HelperApps(), InitErrorFor(res)));
	for (const std::string& ext : availableExtensions)
		OOVR_LOGF("Extension: %s", ext.c_str());

	// Overlay and background applications: a session of their kind, or a refusal. A background application joins only
	// while another OpenLamina program runs: making the instance can start the runtime (Monado's service on Windows).
	const bool helperApps = oovr_global_configuration.HelperApps();
	bool programRunning = false;
	if (appType == vr::VRApplication_Background && helperApps && !BackgroundAppsGetSceneSessions()) {
		SharedStore& store = SharedStore::Get();
		programRunning = SharedStore::OtherLiveProgram(store.Read(StoreDoc::Scene), store.Read(StoreDoc::Overlays), oc_current_pid());
	}
	const SessionChoice choice = ChooseSessionKind(appType, availableExtensions, helperApps, BackgroundAppsGetSceneSessions(), programRunning);
	if (!choice.ok) {
		const char* why = !helperApps                                  ? "helperApps is off"
		    : appType == vr::VRApplication_Background && !programRunning ? "no other OpenLamina program is running"
		                                                                 : "the OpenXR runtime has no overlay or headless sessions for it";
		OOVR_LOGF("Refusing application type %d: %s", (int)appType, why);
		return StartupFailed(error, choice.refusal);
	}
	SetCurrentSessionKind(choice.kind);
	// A helper program whose session can't be started gets the refusal it gets without a runtime: a background
	// application waits on 121 for a VR system to start
	auto failed = [&](vr::EVRInitError e) {
		return StartupFailed(error, choice.kind == SessionKind::Scene ? e : RefusalWithoutSession(appType, true, e));
	};
	if (choice.kind != SessionKind::Scene)
		OOVR_LOGF("Application type %d gets an OpenXR %s session", (int)appType, choice.kind == SessionKind::Overlay ? "overlay" : "headless");

	// The layers are only logged, so failing to list them doesn't matter
	uint32_t availableLayersCount = 0;
	if (XR_FAILED(xrEnumerateApiLayerProperties(0, &availableLayersCount, nullptr)))
		availableLayersCount = 0;
	OOVR_LOGF("Num layers available: %d ", availableLayersCount);
	std::vector<XrApiLayerProperties> layerProperties;
	layerProperties.resize(availableLayersCount, { XR_TYPE_API_LAYER_PROPERTIES });
	if (availableLayersCount && XR_FAILED(xrEnumerateApiLayerProperties(layerProperties.size(), &availableLayersCount, layerProperties.data())))
		layerProperties.clear();
	std::set<std::string> availableLayers;
	for (const XrApiLayerProperties& layer : layerProperties) {
		availableLayers.insert(layer.layerName);
		OOVR_LOGF("Layer: %s", layer.layerName);
	}

	// Create the OpenXR instance - this is the overall handle that connects us to the runtime, or take the one
	// VR_IsHmdPresent made for a scene application
	const InstancePlan plan = PlanInstance(availableExtensions, choice.kind);
	const std::vector<const char*>& extensions = plan.extensions;
	const XrGraphicsApiSupportedFlags apiFlags = plan.apiFlags;
	xr_instance = TakeProbedInstance(plan, &xr_instance_version);
	if (!xr_instance) {
		if (XrResult res = MakeInstance(plan, &xr_instance, &xr_instance_version); XR_FAILED(res)) {
			OOVR_LOGF("WARNING: Instance creation failed: XrResult %d", res);
			if (res == XR_ERROR_RUNTIME_FAILURE) {
				OOVR_LOGF("Is your OpenXR runtime running?");
			}
			xr_instance = XR_NULL_HANDLE;
			return failed(InitErrorFor(res));
		}
	}

#ifdef _DEBUG
	XrDebugUtilsMessengerCreateInfoEXT dbgCreateInfo{};
	dbgCreateInfo.type = XR_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
	dbgCreateInfo.messageSeverities = XR_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
	dbgCreateInfo.messageTypes = XR_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_TYPE_CONFORMANCE_BIT_EXT;
	dbgCreateInfo.next = NULL;
	dbgCreateInfo.userData = NULL;
	if (dbgMessenger != NULL) {
		xrDestroyDebugUtilsMessengerEXT(dbgMessenger);
		dbgMessenger = NULL;
	}
	dbgCreateInfo.userCallback = debugCallback;
	// Debug builds only, and only for logging
	if (XR_FAILED(xrCreateDebugUtilsMessengerEXT(xr_instance, &dbgCreateInfo, &dbgMessenger)))
		dbgMessenger = NULL;
#endif

	// Load the function pointers for the extension functions (in place of the last start-up's, whose instance is gone)
	delete xr_ext;
	xr_ext = new XrExt(apiFlags, extensions);

	if (XrResult res = CreateSystemID(); XR_FAILED(res))
		return failed(InitErrorFor(res));

	// List off the views and store them locally for easy access
	uint32_t viewCount = 0;
	if (XrResult res = xrEnumerateViewConfigurationViews(xr_instance, xr_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr); XR_FAILED(res)) {
		OOVR_LOGF("xrEnumerateViewConfigurationViews failed: %d", (int)res);
		return failed(InitErrorFor(res));
	}

	xr_views_list = std::vector<XrViewConfigurationView>(viewCount, { XR_TYPE_VIEW_CONFIGURATION_VIEW });

	if (XrResult res = xrEnumerateViewConfigurationViews(xr_instance, xr_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, xr_views_list.data()); XR_FAILED(res)) {
		OOVR_LOGF("xrEnumerateViewConfigurationViews failed: %d", (int)res);
		return failed(InitErrorFor(res));
	}

	if (viewCount != XruEyeCount || viewCount != xr_views_list.size()) {
		// A stereo headset has two views; anything else isn't one OpenVR games can draw for
		OOVR_LOGF("The OpenXR runtime has %u primary stereo views, not 2", viewCount);
		return failed(vr::VRInitError_Init_HmdNotFound);
	}

	// Create a session - this tells the runtime that sooner or later we'd like to submit frames
	// This is when we have to choose what graphics API to use

	// An overlay application's images from memory (SetOverlayRaw) go to D3D11 on the runtime's adapter, where the runtime
	// has it; a headless session has no graphics at all
	bool useD3D11TmpGfx = (apiFlags & XR_SUPPORTED_GRAPHICS_API_D3D11) && choice.kind != SessionKind::Headless;
	bool useVulkanTmpGfx = (apiFlags & XR_SUPPORTED_GRAPHICS_API_VK) && choice.kind != SessionKind::Headless
	    && (choice.kind == SessionKind::Overlay ? !useD3D11TmpGfx : oovr_global_configuration.InitUsingVulkan());

#if !defined(SUPPORT_VK) && !defined(SUPPORT_DX) && !defined(SUPPORT_DX11)
#error No available temporary graphics implementation
#endif

	// Build a backend that works with OpenXR
	currentBackend = new XrBackend(useVulkanTmpGfx, useD3D11TmpGfx);
	if (!currentBackend->HasTemporaryGraphics() && choice.kind != SessionKind::Headless) {
		OOVR_LOG("No graphics API to start the OpenXR session with (the runtime offers neither D3D11 nor Vulkan)");
		return failed(vr::VRInitError_Init_Internal);
	}

	// Setup our OpenXR session. An overlay application's stays on OpenLamina's own device for good (its textures are
	// copied onto it), so that its frames never use the program's device.
	if (choice.kind == SessionKind::Overlay) {
		if (!currentBackend->EnsureOverlaySession())
			return failed(vr::VRInitError_Init_Internal);
	} else if (XrResult res = SetupSession(); XR_FAILED(res)) {
		return failed(InitErrorFor(res));
	}
	handleStartupInfo(startupInfo);

	return currentBackend;
}

XrResult DrvOpenXR::SetupSession()
{
	// SetupSession is used to restart the session, and as such, we want to prevent other threads from attempting to use the session
	// while we're rebuilding it (otherwise we get gross nondescript crashes), so we will put a lock on it here.
	auto lock = xr_session.lock();
	if (xr_gbl) {
		ShutdownSession();
	}

	XrSessionCreateInfo sessionInfo{};
	sessionInfo.type = XR_TYPE_SESSION_CREATE_INFO;
	sessionInfo.systemId = xr_system;
	sessionInfo.next = helperChain.Chain(CurrentSessionKind(), CurrentSessionKind() == SessionKind::Headless ? nullptr : currentBackend->GetCurrentGraphicsBinding(),
	    oovr_global_configuration.OverlayLayersPlacement());
	if (XrResult res = xrCreateSession(xr_instance, &sessionInfo, &xr_session.get()); XR_FAILED(res)) {
		// No session: the caller decides (falls back to the temporary device, or marks the session lost)
		OOVR_LOGF("xrCreateSession failed: %d", (int)res);
		xr_session.reset();
		return res;
	}

	// Setup the OpenXR globals, which uses the current session so we have to do this last. Its seated origin is the
	// seated zero pose kept in the store, where one is.
	xr_gbl = new XrSessionGlobals();
	BaseSystem::_LoadStoredSeatedZero();

	// Print the current version for diagnostic purposes
	OOVR_LOGF("Started OpenXR session on runtime '%s', hand tracking supported: %d",
	    xr_gbl->systemProperties.systemName, xr_gbl->handTrackingProperties.supportsHandTracking);

	// If required, re-setup the input system for this new session
	BaseInput* input = GetUnsafeBaseInput();
	if (input)
		input->BindInputsForSession();

	currentBackend->OnSessionCreated();
	return XR_SUCCESS;
}

void DrvOpenXR::KeepOverlaysInstance()
{
	if (currentBackend)
		currentBackend->KeepOverlaysInstance();
}

void DrvOpenXR::StopOwnFrames()
{
	if (currentBackend)
		currentBackend->StopOwnFrames(true);
}

void DrvOpenXR::ShutdownSession()
{
	// No frames of OpenLamina's own on a session that's ending
	if (currentBackend)
		currentBackend->StopOwnFrames();

	BackendManager* instance = BackendManager::InstancePtr();
	// Is it already being shut down?
	// Note that this is indirectly called by the XrBackend destructor, which will have
	// already called this function in that case.
	if (instance) {
		auto* backend = (XrBackend*)instance->GetBackendInstance();
		if (backend)
			backend->PrepareForSessionShutdown();
	}

	delete xr_gbl;
	xr_gbl = nullptr;

	if (currentBackend->sessionActive) {
		currentBackend->ExpectSessionExit();
		// Hey it turns out that xrDestroySession can be called whenever - how convenient
		if (XR_FAILED(xrRequestExitSession(xr_session.get())))
			OOVR_LOG("xrRequestExitSession failed: destroying the session anyway");
		if (!currentBackend->WaitForEvents([&]() { return currentBackend->GetSessionState() == XR_SESSION_STATE_EXITING; }, 2500))
			OOVR_LOG("Session Exit state has not been reached: destroying the session anyway");
	}
	// No running session from here until the next one reaches READY, also if xrCreateSession then fails
	currentBackend->sessionActive = false;

	if (xr_session.get() && XR_FAILED(xrDestroySession(xr_session.get())))
		OOVR_LOG("xrDestroySession failed");
	xr_session.reset();
	currentBackend->sessionState = XR_SESSION_STATE_UNKNOWN;

	CreateSystemID();
}

// Called by ~XrBackend, under its teardown lock
void DrvOpenXR::FullShutdown()
{
	if (xr_session.get())
		ShutdownSession();

#ifdef _DEBUG
	if (dbgMessenger != NULL) {
		xrDestroyDebugUtilsMessengerEXT(dbgMessenger);
		dbgMessenger = NULL;
	}
#endif

	if (xr_instance) {
		if (XR_FAILED(xrDestroyInstance(xr_instance)))
			OOVR_LOG("xrDestroyInstance failed");
		xr_instance = XR_NULL_HANDLE;
	}

	initialised = false;
	currentBackend = nullptr;
	SetCurrentSessionKind(SessionKind::Scene);
}
