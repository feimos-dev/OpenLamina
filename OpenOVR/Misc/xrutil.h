#pragma once

#include <chrono>

#include "generated/interfaces/vrtypes.h"
#include <array>
#include <atomic>
#include <mutex>
#include <optional>
#include <openxr/openxr.h>
#include <shared_mutex>
#include <unordered_map>
#include <vector>
// Note: Xru stands for openXR Utilities

/**
 * Unfortunately OpenXR doesn't define it's own eye constants, but since we know the order
 * for stereoscopic displays (only one of OpenXR's targets, hence why there's no enum for it) we
 * can just define our own.
 *
 * See OpenXR spec section 8.1
 * https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#view_configuration_type
 */
enum XruEye : int {
	XruEyeLeft = 0,
	XruEyeRight = 1,
	XruEyeCount = 2,
};

struct XruCachedViews {
	XrViewState viewState{};
	uint32_t viewCount{};
	std::array<XrView, XruEyeCount> views{};
};

// A macro to ifndef against for a section being ported, so we can remove it to make sure we
// haven't missed anything
#define OC_XR_PORT

#define XR_STUBBED() \
	OOVR_ABORTF("Hit not-yet-ported-to-OpenXR stubbed function at %s:%d func %s", __FILE__, __LINE__, __func__);

#define LINUX_STUBBED() \
	OOVR_ABORTF("Hit not-yet-ported-to-Linux stubbed function at %s:%d func %s", __FILE__, __LINE__, __func__);

// Useful maths typ
// NOTE: THESE ARE DEPRECATED, REMOVE, AND USE THE GLM NAMES
#include <glm/gtc/type_ptr.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
typedef glm::mat4 MfMatrix4f;
typedef glm::vec3 MfVector3f;

class XrSessionGlobals {
public:
	XrSessionGlobals();

	XrSpace floorSpace = XR_NULL_HANDLE;
	// The seated origin: the seated zero pose in the standing space (xr_seated_zero) where one is set, else the
	// runtime's local space
	XrSpace seatedSpace = XR_NULL_HANDLE;
	XrSpace viewSpace = XR_NULL_HANDLE;
	// The reference space floorSpace was made from (StandingSpaceTypes); XR_REFERENCE_SPACE_TYPE_MAX_ENUM if none
	XrReferenceSpaceType floorSpaceType = XR_REFERENCE_SPACE_TYPE_MAX_ENUM;
	/** Moves the seated origin to zero, a pose in the standing space; false (the old origin stays) if the runtime
	 * won't make the space. The caller holds the session lock exclusively. */
	bool SetSeatedZero(const XrPosef& zero);
	// The reference spaces the standing space is made from, in the order they're tried
	static std::vector<XrReferenceSpaceType> StandingSpaceTypes(bool localFloor);

	XrSystemProperties systemProperties = { XR_TYPE_SYSTEM_PROPERTIES };
	XrSystemHandTrackingPropertiesEXT handTrackingProperties = { XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT };
	XrSystemEyeGazeInteractionPropertiesEXT eyeGazeProperties = { XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT };
	// XR_EXT_user_presence's system property: the system tells when the headset is put on or taken off
	bool supportsUserPresence = false;

	// Set by XrBackend, by the thread that runs the frame; read by any thread that asks for a pose
	std::atomic<XrTime> nextPredictedFrameTime{ 1 };

	/**
	 * The latest time we've observed from the runtime. This will be set before a frame is submitted, so for
	 * stuff that needs a time (but it probably doesn't matter much) this can be used.
	 *
	 * For stuff where the next frame time is ideal but some valid time is required, see GetBestTime().
	 *
	 * Note that due to poor implementation, this may lag a long way before nextPredictedFrameTime.
	 *
	 * Zero or negative values for XrTime will result in XR_ERROR_TIME_INVALID so initialise to 1.
	 */
	std::atomic<XrTime> latestTime{ 1 };

	/**
	 * Returns nextPredictedFrameTime if available, otherwise returns latestTime.
	 */
	XrTime GetBestTime();

	/**
	 * Returns a XruCachedViews containing the cached result of a xrLocateViews call with the specified space.
	 */
	XruCachedViews GetCachedViews(XrSpace space);

	/**
	 * Discard all cached xrLocateViews results. This invalidates the references returned by GetCachedViews.
	 */
	void ClearCachedViews();

	/**
	 * What xrEnumerateSwapchainFormats lists for this session, in the runtime's order of preference: asked once, at
	 * the first call (empty if the runtime can't say).
	 */
	const std::vector<int64_t>& SwapchainFormats();

private:
	std::mutex swapchainFormatsMtx;
	bool swapchainFormatsKnown = false;
	std::vector<int64_t> swapchainFormats;

	/**
	 * Makes sure calls involving the cached views map are guarded, since maps are not thread-safe.
	 */
	std::mutex cachedViewsMtx{};

	std::unordered_map<XrSpace, XruCachedViews> cachedViews{};
};

class SessionLock;
// A wrapper around the XrSession to prevent session destruction while it's being accessed
class SessionWrapper {

	std::shared_mutex mutex;
	XrSession session = XR_NULL_HANDLE;
	friend SessionLock;

public:
	/*
	 * Gets a reference to the underlying XrSession that won't be changed while in use.
	 */
	XrSession& get();

	/*
	 * Provides a RAII-style lock on accesses to the XrSession. Prevents any other thread from obtaining get() or lock_shared().
	 */
	SessionLock lock();

	/*
	 * Provides a RAII-style lock on modification to the XrSession. Prevents any other thread from obtaining lock().
	 */
	[[nodiscard]] SessionLock lock_shared();

	/*
	 * The exclusive lock for tearing down, waiting at most limit: when a game exits without VR_Shutdown, Windows has
	 * already ended its other threads, and one may have died holding the lock. Without it, carries on unlocked, the
	 * thread counted as holding it so that requests nested in the teardown don't wait either.
	 * A thread that holds the lock already (shared or exclusive) gets nothing more: no upgrade, unlike lock().
	 */
	[[nodiscard]] SessionLock lock_for_teardown(std::chrono::milliseconds limit);

	/*
	 * The shared lock if it's free now, without waiting (see SessionLock::IsHeld): for a thread that must not wait while a
	 * session restart, which holds the lock exclusively, stops it.
	 */
	[[nodiscard]] SessionLock try_lock_shared();

	/*
	 * The exclusive lock if it's free now, without waiting (see SessionLock::IsHeld)
	 */
	[[nodiscard]] SessionLock try_lock();

	/*
	 * Resets the underlying XrSession to XR_NULL_HANDLE. Does not actually call xrDestroySession.
	 */
	void reset();
};

// This class is to prevent having to litter mutexes everywhere.
// It gets a shared_lock or lock on the session mutex and the mutex gets unlocked (on destruction)
// after whatever function call needs the session completes.
// A request on a thread that already holds the lock is a no-op, except an exclusive request under a shared hold: that
// releases the shared hold, waits for the exclusive lock, and takes the shared lock back when it ends. Whatever the
// outer holder read from the session before may have been replaced by then.
class SessionLock {
	enum class Held { Unlocked, Shared, Exclusive };

	std::shared_lock<std::shared_mutex> shared_lock;
	std::unique_lock<std::shared_mutex> lock;
	SessionWrapper& parent;
	bool owner = false;
	bool upgraded = false;
	inline static thread_local Held thread_lock = Held::Unlocked;

public:
	struct TryShared {};
	struct TryExclusive {};

	SessionLock(SessionWrapper& p, bool exclusive);
	SessionLock(SessionWrapper& p, std::chrono::milliseconds limit);
	SessionLock(SessionWrapper& p, TryShared);
	SessionLock(SessionWrapper& p, TryExclusive);
	~SessionLock();
	operator XrSession&();
	// Whether this thread holds the lock, shared or exclusive, through this SessionLock or an outer one
	bool IsHeld() const { return thread_lock != Held::Unlocked; }
	// Whether this thread holds it exclusively: what a try for the exclusive lock is asked after, since under a shared
	// hold of the thread's own the try takes nothing and IsHeld is true all the same
	bool IsExclusive() const { return thread_lock == Held::Exclusive; }
};

// The runtime's time now, through XR_KHR_win32_convert_performance_counter_time (Windows) or
// XR_KHR_convert_timespec_time (elsewhere) where the runtime has it (it's enabled for every session); 0 without
XrTime xr_time_now();

XrSpace xr_space_from_tracking_origin(vr::ETrackingUniverseOrigin origin);
XrSpace xr_space_from_ref_space_type(XrReferenceSpaceType spaceType);

// xrSuggestInteractionProfileBindings, with the runtime's answer for the profile logged: it takes a profile's
// bindings all or none
XrResult xr_suggest_bindings_logged(const XrInteractionProfileSuggestedBinding& suggested);

XrQuaternionf yRotation(XrQuaternionf& q, XrQuaternionf& r);
void rotate_vector_by_quaternion(const XrVector3f& v, const XrQuaternionf& q, XrVector3f& vprime);

extern XrInstance xr_instance;
extern SessionWrapper& xr_session;
extern XrSystemId xr_system;
XrViewConfigurationView& xr_main_view(XruEye view_id);
extern XrSessionGlobals* xr_gbl;
// The seated zero pose in the standing space, once one is set (ResetSeatedZeroPose, a room setup or the store's): each
// session's seated origin. Written with the session lock held exclusively.
extern std::optional<XrPosef> xr_seated_zero;

class XrExt;
extern XrExt* xr_ext;
