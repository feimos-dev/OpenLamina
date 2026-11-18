#pragma once
// Pointing at overlays with the controllers, as a mouse: each hand's laser (its aim pose), the trigger as the left
// button and the thumbstick as a scroll wheel, turned into the mouse, scroll and focus events an overlay with
// VROverlayInputMethod_Mouse gets. Pure: the caller reads the hands and places the overlays.

#include "OverlayMaths.h"
#include "generated/interfaces/vrtypes.h"

#include <openxr/openxr.h>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

// One hand's laser
struct PointerRay {
	int hand; // 0 the left, 1 the right
	XrVector3f origin, dir; // dir needn't be unit length
	bool click; // the trigger is held
	float scrollX, scrollY; // the thumbstick, -1 to 1
	bool valid; // the hand is tracked
	vr::TrackedDeviceIndex_t device = vr::k_unTrackedDeviceIndexInvalid; // the events' trackedDeviceIndex
};

// Something to point at: an overlay, or what OpenLamina draws itself (the keyboard, a message box)
struct PointerTarget {
	vr::VROverlayHandle_t handle;
	overlay_maths::Placement placement;
	std::vector<OOVR_VROverlayIntersectionMaskPrimitive_t> mask; // in mouse units (see yDown); empty: all of it
	vr::HmdVector2_t mouseScale; // the mouse units across and up it (down it, with yDown)
	// OpenVR's mouse units start at the lower left of the overlay (y up). What OpenLamina draws itself (the keyboard,
	// a message box) counts from the upper left, as its canvas does.
	bool yDown = false;
	uint32_t flags; // VROverlayFlags, as masks
	int sortOrder; // the higher is drawn on top
};

struct PointerEvent {
	vr::VROverlayHandle_t handle; // the target the event is for
	vr::VREvent_t event;
};

// VROverlayFlags as masks: scroll events by steps, or smoothly
constexpr uint32_t kOverlayFlagDiscreteScroll = 1u << 6;
constexpr uint32_t kOverlayFlagSmoothScroll = 1u << 17;
// No cursor drawn where the laser meets the overlay
constexpr uint32_t kOverlayFlagHideLaserIntersection = 1u << 19;

class LaserMouse {
public:
	// Each hand's nearest hit (the highest sort order, then the nearest) is where it points. One hand drives the
	// mouse at a time: the last to press its trigger on a target (the right to begin with, or whichever points at
	// something while the other doesn't). Its target gets FocusEnter and FocusLeave as it changes, MouseMove (x, y =
	// u * scale.x, (1 - v) * scale.y) when it moves, MouseButtonDown and Up (Left) for its trigger, and ScrollDiscrete
	// or ScrollSmooth for its thumbstick, as the target's flags ask. A click belongs to the hand that started it until
	// that hand lets go; it stays on the target it started on.
	void Update(const std::array<PointerRay, 2>& rays, const std::vector<PointerTarget>& targets, double now, std::vector<PointerEvent>& out);

	// Where a hand's laser meets its target (for the cursor), nothing if it points at none
	std::optional<overlay_maths::Hit> Cursor(int hand) const;
	// The target a hand points at, k_ulOverlayHandleInvalid for none
	vr::VROverlayHandle_t Target(int hand) const;
	// The hand that drives the mouse
	int ActiveHand() const { return active; }
	// The hand pointing at a target, the one driving the mouse first; -1 if none is
	int HandOn(vr::VROverlayHandle_t handle) const;
	// The target with the mouse's focus (k_ulOverlayHandleInvalid for none)
	vr::VROverlayHandle_t Focus() const { return focus; }

private:
	struct Hand {
		vr::VROverlayHandle_t target = vr::k_ulOverlayHandleInvalid;
		std::optional<overlay_maths::Hit> hit;
		float x = 0, y = 0; // on the target, in its mouse units
		bool held = false; // the trigger, at the last update
		double nextScroll = 0; // a held thumbstick's next scroll step
	};

	std::array<Hand, 2> hands;
	int active = 1;
	int clickOwner = -1;
	vr::VROverlayHandle_t clickTarget = vr::k_ulOverlayHandleInvalid;
	vr::VROverlayHandle_t focus = vr::k_ulOverlayHandleInvalid;
	vr::VROverlayHandle_t movedOn = vr::k_ulOverlayHandleInvalid; // the target of the last MouseMove, and where
	float movedX = 0, movedY = 0;
	double lastTime = -1;
};
