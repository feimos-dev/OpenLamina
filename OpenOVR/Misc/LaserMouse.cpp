#include "stdafx.h"

#include "LaserMouse.h"

#include <algorithm>
#include <cmath>

using namespace overlay_maths;

namespace {

// A thumbstick pushed less than this doesn't scroll
constexpr float kScrollDeadzone = 0.1f;
// Discrete scrolling: a step once the stick is pushed this far, then one every kScrollRepeat seconds while it stays
constexpr float kScrollStepAt = 0.5f;
constexpr double kScrollRepeat = 0.25;
// Smooth scrolling: units per second at full deflection
constexpr float kSmoothScrollRate = 8.0f;

const PointerTarget* Find(const std::vector<PointerTarget>& targets, vr::VROverlayHandle_t handle)
{
	for (const PointerTarget& t : targets)
		if (t.handle == handle)
			return &t;
	return nullptr;
}

// The mouse's y for a texture coordinate v (0 at the bottom): up from the lower left, or down from the upper left
float MouseY(const PointerTarget& t, float v)
{
	return (t.yDown ? 1 - v : v) * t.mouseScale.v[1];
}

vr::VREvent_t MakeEvent(uint32_t type, vr::TrackedDeviceIndex_t device)
{
	vr::VREvent_t e{};
	e.eventType = type;
	e.trackedDeviceIndex = device;
	return e;
}

} // namespace

void LaserMouse::Update(const std::array<PointerRay, 2>& rays, const std::vector<PointerTarget>& targets, double now, std::vector<PointerEvent>& out)
{
	const double dt = lastTime < 0 ? 0 : std::clamp(now - lastTime, 0.0, 0.1);
	lastTime = now;

	// Where each hand points
	for (int h = 0; h < 2; h++) {
		Hand& hand = hands[h];
		const PointerRay& ray = rays[h];
		hand.target = vr::k_ulOverlayHandleInvalid;
		hand.hit.reset();
		if (!ray.valid)
			continue;
		if (clickOwner == h) {
			// A click stays on its target: the hand's laser on that target's plane, even past its edge
			if (const PointerTarget* t = Find(targets, clickTarget)) {
				Hit hit{};
				hit.distance = -1;
				Intersect(t->placement, ray.origin, ray.dir, hit);
				if (hit.distance >= 0) {
					hand.target = clickTarget;
					hand.hit = hit;
					hand.x = hit.u * t->mouseScale.v[0];
					hand.y = MouseY(*t, hit.v);
				}
			}
			continue;
		}
		const PointerTarget* best = nullptr;
		Hit bestHit{};
		for (const PointerTarget& t : targets) {
			Hit hit{};
			if (!Intersect(t.placement, ray.origin, ray.dir, hit))
				continue;
			const float x = hit.u * t.mouseScale.v[0];
			const float y = MouseY(t, hit.v);
			if (!InMask(t.mask, x, y))
				continue;
			if (best && (t.sortOrder < best->sortOrder || (t.sortOrder == best->sortOrder && hit.distance >= bestHit.distance)))
				continue;
			best = &t;
			bestHit = hit;
		}
		if (best) {
			hand.target = best->handle;
			hand.hit = bestHit;
			hand.x = bestHit.u * best->mouseScale.v[0];
			hand.y = MouseY(*best, bestHit.v);
		}
	}

	// Which hand drives the mouse: the one holding a click; else the last to press its trigger on a target; else
	// whichever points at something while the other doesn't
	if (clickOwner < 0) {
		for (int h = 0; h < 2; h++) {
			if (rays[h].valid && rays[h].click && !hands[h].held && hands[h].target != vr::k_ulOverlayHandleInvalid) {
				active = h;
				break;
			}
		}
		if (hands[active].target == vr::k_ulOverlayHandleInvalid && hands[1 - active].target != vr::k_ulOverlayHandleInvalid)
			active = 1 - active;
	}
	const Hand& driver = hands[active];
	const vr::TrackedDeviceIndex_t device = rays[active].device;

	// Focus
	const vr::VROverlayHandle_t newFocus = clickOwner >= 0 ? clickTarget : driver.target;
	if (newFocus != focus) {
		if (focus != vr::k_ulOverlayHandleInvalid) {
			vr::VREvent_t e = MakeEvent(vr::VREvent_FocusLeave, device);
			e.data.overlay.overlayHandle = focus;
			out.push_back({ focus, e });
		}
		if (newFocus != vr::k_ulOverlayHandleInvalid) {
			vr::VREvent_t e = MakeEvent(vr::VREvent_FocusEnter, device);
			e.data.overlay.overlayHandle = newFocus;
			out.push_back({ newFocus, e });
		}
		focus = newFocus;
	}

	// Moving
	const bool onTarget = driver.target != vr::k_ulOverlayHandleInvalid;
	if (onTarget && (movedOn != driver.target || movedX != driver.x || movedY != driver.y)) {
		vr::VREvent_t e = MakeEvent(vr::VREvent_MouseMove, device);
		e.data.mouse.x = driver.x;
		e.data.mouse.y = driver.y;
		e.data.mouse.button = clickOwner >= 0 ? vr::VRMouseButton_Left : 0;
		out.push_back({ driver.target, e });
		movedOn = driver.target;
		movedX = driver.x;
		movedY = driver.y;
	}

	// The click: pressed on a target, released wherever
	if (clickOwner < 0) {
		const bool pressed = rays[active].valid && rays[active].click && !driver.held;
		if (pressed && onTarget) {
			clickOwner = active;
			clickTarget = driver.target;
			vr::VREvent_t e = MakeEvent(vr::VREvent_MouseButtonDown, device);
			e.data.mouse.x = driver.x;
			e.data.mouse.y = driver.y;
			e.data.mouse.button = vr::VRMouseButton_Left;
			out.push_back({ clickTarget, e });
		}
	} else if (!rays[clickOwner].valid || !rays[clickOwner].click) {
		const Hand& owner = hands[clickOwner];
		vr::VREvent_t e = MakeEvent(vr::VREvent_MouseButtonUp, rays[clickOwner].device);
		e.data.mouse.x = owner.x;
		e.data.mouse.y = owner.y;
		e.data.mouse.button = vr::VRMouseButton_Left;
		out.push_back({ clickTarget, e });
		clickOwner = -1;
		clickTarget = vr::k_ulOverlayHandleInvalid;
	}

	// Scrolling, on the focused target as its flags ask
	if (const PointerTarget* t = Find(targets, focus); t && rays[active].valid) {
		const float sx = rays[active].scrollX, sy = rays[active].scrollY;
		Hand& hand = hands[active];
		if (t->flags & kOverlayFlagSmoothScroll) {
			const float x = std::abs(sx) >= kScrollDeadzone ? sx : 0.0f;
			const float y = std::abs(sy) >= kScrollDeadzone ? sy : 0.0f;
			if ((x != 0 || y != 0) && dt > 0) {
				vr::VREvent_t e = MakeEvent(vr::VREvent_ScrollSmooth, device);
				e.data.scroll.xdelta = x * (float)dt * kSmoothScrollRate;
				e.data.scroll.ydelta = y * (float)dt * kSmoothScrollRate;
				e.data.scroll.viewportscale = 1.0f;
				out.push_back({ focus, e });
			}
		} else if (t->flags & kOverlayFlagDiscreteScroll) {
			const bool pushed = std::abs(sx) >= kScrollStepAt || std::abs(sy) >= kScrollStepAt;
			if (!pushed) {
				hand.nextScroll = 0;
			} else if (now >= hand.nextScroll) {
				vr::VREvent_t e = MakeEvent(vr::VREvent_ScrollDiscrete, device);
				e.data.scroll.xdelta = std::abs(sx) >= kScrollStepAt ? (sx > 0 ? 1.0f : -1.0f) : 0.0f;
				e.data.scroll.ydelta = std::abs(sy) >= kScrollStepAt ? (sy > 0 ? 1.0f : -1.0f) : 0.0f;
				e.data.scroll.viewportscale = 1.0f;
				out.push_back({ focus, e });
				hand.nextScroll = now + kScrollRepeat;
			}
		}
	}

	for (int h = 0; h < 2; h++)
		hands[h].held = rays[h].valid && rays[h].click;
}

std::optional<Hit> LaserMouse::Cursor(int hand) const
{
	if (hand < 0 || hand > 1)
		return {};
	return hands[hand].hit;
}

vr::VROverlayHandle_t LaserMouse::Target(int hand) const
{
	if (hand < 0 || hand > 1)
		return vr::k_ulOverlayHandleInvalid;
	return hands[hand].target;
}

int LaserMouse::HandOn(vr::VROverlayHandle_t handle) const
{
	if (handle == vr::k_ulOverlayHandleInvalid)
		return -1;
	if (hands[active].target == handle)
		return active;
	if (hands[1 - active].target == handle)
		return 1 - active;
	return -1;
}
