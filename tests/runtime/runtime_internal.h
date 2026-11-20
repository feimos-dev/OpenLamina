#pragma once
// Internals of the stand-in OpenXR runtime (oc_test_runtime).

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11.h>
#include <d3d12.h>
#include <GL/gl.h>
#else
// Xlib and GLX for XR_KHR_opengl_enable's Xlib binding. X11's macros that clash with names here are undefined after.
#include <GL/glx.h>
#include <X11/Xlib.h>
#undef None
#undef Bool
#undef Status
#undef Success
#undef Always
#endif

// Vulkan's functions are looked up at run time (the loader needn't exist for the other APIs)
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

// Windows: D3D11, D3D12, Vulkan and OpenGL (WGL). Elsewhere: Vulkan and OpenGL (GLX). Not OpenGL ES.
#undef XR_USE_GRAPHICS_API_OPENGL_ES
#ifdef _WIN32
#ifndef XR_USE_PLATFORM_WIN32
#define XR_USE_PLATFORM_WIN32
#endif
#ifndef XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D11
#endif
#ifndef XR_USE_GRAPHICS_API_D3D12
#define XR_USE_GRAPHICS_API_D3D12
#endif
#else
#ifndef XR_USE_PLATFORM_XLIB
#define XR_USE_PLATFORM_XLIB
#endif
#ifndef XR_USE_TIMESPEC
#define XR_USE_TIMESPEC
#endif
#include <time.h>
#endif
#ifndef XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_VULKAN
#endif
#ifndef XR_USE_GRAPHICS_API_OPENGL
#define XR_USE_GRAPHICS_API_OPENGL
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <loader_interfaces.h>

#include "script.h"

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

// --- poses ---

inline XrQuaternionf QMul(const XrQuaternionf& a, const XrQuaternionf& b)
{
	return {
		a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
		a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
		a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
		a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
	};
}

inline XrQuaternionf QConj(const XrQuaternionf& q) { return { -q.x, -q.y, -q.z, q.w }; }

inline XrVector3f QRotate(const XrQuaternionf& q, const XrVector3f& v)
{
	XrQuaternionf p{ v.x, v.y, v.z, 0 };
	XrQuaternionf r = QMul(QMul(q, p), QConj(q));
	return { r.x, r.y, r.z };
}

inline XrPosef Compose(const XrPosef& a, const XrPosef& b) // a * b: b expressed in a's frame
{
	XrVector3f p = QRotate(a.orientation, b.position);
	return { QMul(a.orientation, b.orientation), { a.position.x + p.x, a.position.y + p.y, a.position.z + p.z } };
}

inline XrPosef Inverse(const XrPosef& a)
{
	XrQuaternionf inv = QConj(a.orientation);
	XrVector3f p = QRotate(inv, a.position);
	return { inv, { -p.x, -p.y, -p.z } };
}

inline const XrPosef kIdentity{ { 0, 0, 0, 1 }, { 0, 0, 0 } };
inline const XrPosef kHead{ { 0, 0, 0, 1 }, { 0, 1.7f, 0 } };
// The HMD in STAGE: the script's "/user/head" pose at the current frame, else kHead
XrPosef RtHeadPose();

// Grip and aim pose of a hand in STAGE (both the same), 0 = left, 1 = right
inline XrPosef HandPose(int hand) { return { { 0, 0, 0, 1 }, { hand == 0 ? -0.2f : 0.2f, 1.4f, -0.3f } }; }

// --- objects ---

struct RtActionSet {
	std::string name;
	bool attached = false;
};

struct RtAction {
	RtActionSet* set = nullptr;
	std::string name;
	XrActionType type = XR_ACTION_TYPE_BOOLEAN_INPUT;
	std::vector<XrPath> subactionPaths;
};

struct RtSpace {
	XrReferenceSpaceType type = XR_REFERENCE_SPACE_TYPE_LOCAL;
	XrPosef offset = kIdentity;
	bool isAction = false;
	XrPosef actionPose = kIdentity; // pose in STAGE for action spaces
	RtAction* action = nullptr; // for action spaces: the action and subaction path, for the script's poses (a gaze
	                            // space follows the script's gaze instead, RtGazeSpace)
	XrPath subaction = XR_NULL_PATH;
};

enum class RtApi { None, D3D11, D3D12, Vulkan, OpenGL };

// The layout of a swapchain format's texels, whatever the API's enum for it: DepthF32 is a 32-bit float depth format
// (read back, the value in every channel), Depth the others (not read back)
enum class PixelLayout { Unknown, RGBA8, BGRA8, RGBA16F, RGB10A2, Depth, DepthF32 };

inline bool IsDepthLayout(PixelLayout layout) { return layout == PixelLayout::Depth || layout == PixelLayout::DepthF32; }

struct RtSwapchain {
	XrSwapchainCreateInfo info{};
	// ID3D11Texture2D*, ID3D12Resource*, VkImage or GL texture name; Vulkan: each one's VkDeviceMemory in memory
	std::vector<uint64_t> images;
	std::vector<uint64_t> memory;
	uint32_t next = 0;
	int acquired = 0; // acquired and not yet released
	int unwaited = 0; // of those, not yet waited on (the spec: wait before writing, release only waited images)
	std::deque<uint32_t> acquiredOrder; // indices acquired and not yet released, oldest first
	int lastReleased = -1; // the image a layer shows (none released yet: -1)
};

struct RtSession {
	RtApi api = RtApi::None;
#ifdef _WIN32
	ID3D11Device* device = nullptr; // D3D11
	ID3D12Device* d3d12Device = nullptr;
	ID3D12CommandQueue* d3d12Queue = nullptr;
#endif
	VkInstance vkInstance = VK_NULL_HANDLE;
	VkPhysicalDevice vkPhysicalDevice = VK_NULL_HANDLE;
	VkDevice vkDevice = VK_NULL_HANDLE;
	uint32_t vkQueueFamily = 0, vkQueueIndex = 0;
#ifdef _WIN32
	HDC glDC = nullptr;
	HGLRC glRC = nullptr;
#else
	Display* glDisplay = nullptr;
	GLXDrawable glDrawable = 0;
	GLXContext glContext = nullptr;
#endif
	bool headless = false;
	bool overlay = false;
	bool running = false;
	bool exitRequested = false; // xrRequestExitSession: xrEndSession then goes on to EXITING, as the spec says
	XrSessionState state = XR_SESSION_STATE_UNKNOWN;
	uint64_t framesWaited = 0;
	uint64_t framesBegun = 0;
	uint64_t framesEnded = 0;
	bool frameOpen = false;
	bool actionsAttached = false;
	XrPath currentProfile = XR_NULL_PATH; // set at the first xrSyncActions
	std::set<RtActionSet*> activeSets;
	std::map<std::string, float> inputValues; // as of the last xrSyncActions
	float refreshRate = 90;
};

struct RtState {
	std::recursive_mutex lock;
	bool instanceAlive = false;
	std::set<RtApi> requirementsQueried; // xrGet*GraphicsRequirementsKHR called: a session on that API needs it first
#ifdef _WIN32
	LUID requiredLuid{}; // the adapter xrGetD3D11/D3D12GraphicsRequirementsKHR named: a D3D session's device must be on it
#endif
	std::set<std::string> enabledExtensions;
	std::unique_ptr<RtSession> session;
	std::map<RtSpace*, std::unique_ptr<RtSpace>> spaces;
	std::map<RtSwapchain*, std::unique_ptr<RtSwapchain>> swapchains;
	std::map<RtActionSet*, std::unique_ptr<RtActionSet>> actionSets;
	std::map<RtAction*, std::unique_ptr<RtAction>> actions;
	// profile path → (action, binding path) pairs suggested for it
	std::map<XrPath, std::vector<std::pair<RtAction*, XrPath>>> suggested;
	std::deque<XrEventDataBuffer> events;
	std::map<std::string, XrPath> pathIds;
	std::vector<std::string> paths{ "" };
	XrTime time = 1000000000;
	bool presenceSent = false;
};

RtState& St();
XrPath RtPath(const std::string& str); // interns a path (lock held or not)
std::string RtPathString(XrPath path);
void QueueState(RtSession* s, XrSessionState state);
void QueueEvent(const XrEventDataBuffer& ev);
bool RtExtensionEnabled(const char* name);

// The pose in STAGE of a space (action spaces: the script's gaze for a gaze space, else the script's pose for the
// action's binding, if any); *velocity gets the script's velocities in STAGE (zero without one), when not null. Lock
// held.
XrPosef RtSpaceInStage(const RtSpace* sp, ScriptPose* velocity = nullptr);

// One graphics API's part of the runtime (swapchain_d3d11.cpp, swapchain_d3d12.cpp, swapchain_vk.cpp, swapchain_gl.cpp)
struct RtGfx {
	RtApi api;
	const char* binding; // the dump's session.binding
	const char* extension; // the extension that offers it
	const char* extension2; // another one that does (XR_KHR_vulkan_enable2), or nullptr
	XrStructureType bindingType; // its XrGraphicsBinding*KHR (XrGraphicsBindingVulkan2KHR has Vulkan's type)
	XrStructureType imageType; // its XrSwapchainImage*KHR
	// Checks the binding as Monado does and takes the session's device from it (holding a reference where the API
	// counts them); the error to answer xrCreateSession with if it's unusable (nothing held then)
	XrResult (*bind)(const XrBaseInStructure* binding, RtSession* session);
	void (*unbind)(RtSession* session);
	const std::vector<int64_t>& (*formats)(); // the formats it can make
	const std::vector<int64_t>& (*defaultFormats)(); // what xrEnumerateSwapchainFormats lists without a script
	XrResult (*create)(RtSession* session, const XrSwapchainCreateInfo* info, RtSwapchain* sc);
	void (*destroy)(RtSession* session, RtSwapchain* sc);
	void (*enumerate)(const RtSwapchain* sc, XrSwapchainImageBaseHeader* images); // writes sc->images.size() images
	PixelLayout (*layout)(int64_t format);
	bool bottomUp; // the image's row 0 is its bottom one (OpenGL)
	// Reads the stored texels at points (the image's own pixel coordinates) of image index, array layer arrayIndex,
	// through a staging copy on the session's device: bytesPerTexel each, in order, into out; false with why if it can't
	bool (*read)(RtSession* session, RtSwapchain* sc, uint32_t index, uint32_t arrayIndex,
	    const std::vector<XrOffset2Di>& points, uint32_t bytesPerTexel, std::vector<uint8_t>& out, std::string* why);
};
#ifdef _WIN32
const RtGfx& RtD3D11Gfx();
const RtGfx& RtD3D12Gfx();
#endif
const RtGfx& RtVulkanGfx();
const RtGfx& RtOpenGLGfx();
const RtGfx* RtGfxOf(RtApi api); // nullptr for None
const RtGfx* RtGfxForBinding(XrStructureType type);
// The API's functions (requirements, Vulkan's instance and device): *extension gets the extension they need
PFN_xrVoidFunction RtGfxLookup(const char* name, const char** extension);
#ifdef _WIN32
PFN_xrVoidFunction RtD3D11Lookup(const char* name);
PFN_xrVoidFunction RtD3D12Lookup(const char* name);
#endif
PFN_xrVoidFunction RtVulkanLookup(const char* name, const char** extension);
PFN_xrVoidFunction RtOpenGLLookup(const char* name);
uint32_t RtBytesOf(PixelLayout layout);
// A readback whose GPU wait timed out: its objects are left alive (the GPU may still use them), counted in the dump
// as readback_timeouts
void RtNoteReadbackTimeout();
// Whether the app enabled an RtGfx's extension (either of them)
bool RtGfxEnabled(const RtGfx& gfx);

// actions.cpp
PFN_xrVoidFunction RtActionsLookup(const char* name);
// An action space of an action bound to XR_EXT_eye_gaze_interaction's gaze pose (the extension enabled): its pose in
// STAGE now and its location flags (none while the script's gaze is invalid)
bool RtGazeSpace(const RtSpace* sp, XrPosef* poseInStage, XrSpaceLocationFlags* flags);
void RtActionsOnFrame(RtSession* s); // per xrWaitFrame (presence events)
void RtActionsReset();
void RtActionsDump(Json::Value& root);
// The script's pose for an action space (the first binding path of its action, under its subaction path, that has
// one: the current profile's bindings, else any suggested ones), false if none
bool RtActionSpaceScripted(const RtSpace* sp, uint64_t frame, ScriptPose* out);

// extensions.cpp: functions of optional extensions; *extension gets the extension that must be enabled
PFN_xrVoidFunction RtExtensionsLookup(const char* name, const char** extension);
void RtExtensionsSystemProperties(XrSystemProperties* props);
#ifdef _WIN32
// The adapter the D3D11 and D3D12 requirements name: the one whose LUID is OC_TEST_RUNTIME_GPU_LUID (16 hex digits,
// HighPart then LowPart) when that is set and one has it, else the first. False if there's none.
bool RtRuntimeAdapterLuid(LUID* out);
#endif
// The display's refresh rate changes to rate (XR_FB_display_refresh_rate's event, if it changed); with St().lock held
void RtChangeRefreshRate(RtSession* s, float rate);
bool RtExtensionsSessionInfo(const XrSessionCreateInfo* info, RtSession* session); // headless / overlay chains
#ifdef _WIN32
// The system clock as XrTime (XR_KHR_win32_convert_performance_counter_time's conversion): QueryPerformanceCounter in
// nanoseconds
XrTime RtCounterTime(const LARGE_INTEGER& counter);
#else
// The system clock as XrTime (XR_KHR_convert_timespec_time's conversion): CLOCK_MONOTONIC in nanoseconds
XrTime RtTimespecTime(const timespec& ts);
#endif
XrTime RtNowTime();
