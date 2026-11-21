#pragma once
// Pointing at other programs' overlays: the scene app's instance runs the controllers' lasers over the mouse overlays
// every live OpenLamina program has put in the store (the Overlays document), and sends each mouse, scroll and focus
// event to the program that owns the overlay, through the store's event ring. That program queues it on the overlay.
//
// The Overlays document: { "<pid>": { "app": key, "start": the process's start time, "updated": t, "overlays": [{
// "handle", "key", "name", "pose": {"p": [x, y, z], "q": [x, y, z, w]}, "space": "standing", "relative_to": "hmd" |
// "left" | "right" (then the pose is in that device's space), "width", "height", "curvature", "input_method", "flags",
// "sort_order", "visible", "dashboard", "mask": [{"rect": [x, y, w, h]} | {"circle": [x, y, r]}] (in units of the
// overlay's width and height, from its lower left) }] } }, each program writing its own entry.

#include "LaserMouse.h"
#include "ProgramEvents.h"
#include "SharedStore.h"

#include "json/json.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <vector>

// One event for another program's overlay
struct RemotePointerEvent {
	uint32_t targetPid;
	uint64_t handle; // the overlay's handle in its own program
	uint32_t eventType; // the OpenVR event
	float x, y; // mouse events: where on the overlay, 0 to 1 across and up (the owner scales by its mouse scale);
	            // scroll events: the scroll's deltas
	uint32_t button; // VRMouseButton
	vr::TrackedDeviceIndex_t device; // the controller
};

// As it crosses the ring: data[0] the pid and event type, data[1] the handle, data[2] x and y, data[3] the device and
// button (the event's device, as OcPackEvent has it)
StoreEvent MakeRemotePointerEvent(const RemotePointerEvent& e);
RemotePointerEvent UnpackRemotePointer(const uint64_t (&data)[4]);

// The Overlays document as read from the store, with what other programs wrote in another shape left out: an entry
// that isn't an object, and each field above of another type than its own. Read through jsoncpp's converting
// accessors, which throw on a wrong type, every field left is safe to read (a missing one reads as null).
Json::Value CheckedOverlaysDoc(const Json::Value& doc);

// An overlay's "mask" list from the Overlays document: its primitives, in units of the overlay's width and height
std::vector<OOVR_VROverlayIntersectionMaskPrimitive_t> MaskFromJson(const Json::Value& mask);

// Each one to the other programs, in order, without waiting for the store (they're posted from the game's frame): how
// many of them, from the first, were posted. The rest are the caller's to post again.
size_t PostRemotePointerEvents(const std::vector<StoreEvent>& events);

class RemotePointer {
public:
	// The scene app's instance: LaserMouse over every other live program's mouse overlays from the store (its own,
	// selfPid's, are pointed at by its own overlays' pointer, and skipped here). The events for other programs go into
	// toPost.
	void Update(const std::array<PointerRay, 2>& rays, const Json::Value& overlaysDoc, uint32_t selfPid, double now, std::vector<StoreEvent>& toPost);

	// The rays' space: overlays in the document are in the standing space, and are moved into the seated one (by
	// seatedInStanding) for a scene app whose frames are in it
	void SetFrameOrigin(vr::ETrackingUniverseOrigin origin, const XrPosef& seatedInStanding);

	// The HMD's and the hands' poses this frame, in the standing space (none where untracked): overlays relative to
	// them are placed by them, as their owners place them
	void SetDevicePoses(const std::array<std::optional<XrPosef>, 3>& hmdLeftRight);

	// Whether the last Update saw an entry of a program that isn't running any more (the next write drops it)
	bool SawDeadEntries() const { return sawDead; }

	// Where the laser driving the mouse meets another program's overlay, for the cursor
	std::optional<XrPosef> CursorPoint() const;

	// Whether a program (its pid and start time) is running (tests replace it)
	std::function<bool(uint32_t, uint64_t)> isAlive = SharedStore::IsSameProcessAlive;

private:
	LaserMouse mouse;
	// Other programs' overlays (pid, handle) as LaserMouse's handles, kept while they're in the document
	std::map<std::pair<uint32_t, uint64_t>, vr::VROverlayHandle_t> ids;
	std::map<vr::VROverlayHandle_t, std::pair<uint32_t, uint64_t>> owners;
	vr::VROverlayHandle_t nextId = 1;
	std::vector<PointerTarget> lastTargets;
	vr::ETrackingUniverseOrigin origin = vr::TrackingUniverseStanding;
	XrPosef seatedInStanding{ { 0, 0, 0, 1 }, { 0, 0, 0 } };
	std::array<std::optional<XrPosef>, 3> devices;
	bool sawDead = false;
};
