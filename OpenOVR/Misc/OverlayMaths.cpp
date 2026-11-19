#include "stdafx.h"

#include "OverlayMaths.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <set>

namespace overlay_maths {

namespace {

glm::vec3 V(const XrVector3f& v)
{
	return { v.x, v.y, v.z };
}

XrVector3f X(const glm::vec3& v)
{
	return { v.x, v.y, v.z };
}

glm::quat Q(const XrQuaternionf& q)
{
	return glm::normalize(glm::quat(q.w, q.x, q.y, q.z));
}

XrQuaternionf X(const glm::quat& q)
{
	return { q.x, q.y, q.z, q.w };
}

XrPosef Compose(const XrPosef& a, const XrPosef& b)
{
	const glm::quat qa = Q(a.orientation);
	return XrPosef{ X(glm::normalize(qa * Q(b.orientation))), X(V(a.position) + qa * V(b.position)) };
}

XrPosef Inverse(const XrPosef& a)
{
	const glm::quat inv = glm::conjugate(Q(a.orientation));
	return XrPosef{ X(inv), X(inv * -V(a.position)) };
}

// A rotation about the overlay's Y axis
glm::quat AboutY(float angle)
{
	return glm::angleAxis(angle, glm::vec3(0, 1, 0));
}

} // namespace

float CylinderRadius(float width, float curvature)
{
	return width / (glm::two_pi<float>() * std::min(curvature, 1.0f));
}

float CylinderAngle(float curvature)
{
	// OpenXR wants it below 2π, which a float 2π rounds above
	return std::min(glm::two_pi<float>() * std::min(curvature, 1.0f), std::nextafter(glm::two_pi<float>(), 0.0f));
}

bool Intersect(const Placement& p, const XrVector3f& origin, const XrVector3f& dir, Hit& hit)
{
	const float length = glm::length(V(dir));
	if (!(p.width > 0) || !(p.height > 0) || !(length > 0))
		return false;
	// In the overlay's own space: its centre at 0, facing +Z
	const glm::quat q = Q(p.pose.orientation);
	const glm::quat toLocal = glm::conjugate(q);
	const glm::vec3 position = V(p.pose.position);
	const glm::vec3 o = toLocal * (V(origin) - position);
	const glm::vec3 d = toLocal * (V(dir) / length);

	auto fill = [&](float t, const glm::vec3& point, const glm::vec3& normal, float u, float v) {
		hit.distance = t;
		hit.u = u;
		hit.v = v;
		hit.point = X(q * point + position);
		hit.normal = X(q * normal);
		return u > 0 && u < 1 && v > 0 && v < 1;
	};

	if (!(p.curvature > 0)) {
		// A quad: only from its front, and not along it
		if (o.z < 0 || !(d.z < -0.01f))
			return false;
		const float t = -o.z / d.z;
		const glm::vec3 point = o + d * t;
		return fill(t, point, { 0, 0, 1 }, point.x / p.width + 0.5f, point.y / p.height + 0.5f);
	}

	// A cylinder around the overlay's Y axis, a radius in front of it; the overlay is the arc centred on its -Z
	const float radius = CylinderRadius(p.width, p.curvature);
	const float angle = CylinderAngle(p.curvature);
	const glm::vec3 axis(0, 0, radius);
	const glm::vec3 oc = o - axis;
	const float a = d.x * d.x + d.z * d.z;
	if (!(a > 1e-8f))
		return false; // along the axis
	const float b = 2 * (oc.x * d.x + oc.z * d.z);
	const float c = oc.x * oc.x + oc.z * oc.z - radius * radius;
	const float disc = b * b - 4 * a * c;
	if (!(disc >= 0))
		return false;
	const float root = std::sqrt(disc);
	bool filled = false;
	for (float t : { (-b - root) / (2 * a), (-b + root) / (2 * a) }) {
		if (t < 0)
			continue;
		const glm::vec3 fromAxis = oc + d * t;
		const glm::vec3 outwards = glm::vec3(fromAxis.x, 0, fromAxis.z) / radius;
		// The overlay's front is the cylinder's inside: a ray going outwards meets it
		const bool front = glm::dot(d, outwards) > 0;
		const float theta = std::atan2(fromAxis.x, -fromAxis.z);
		const bool onArc = std::abs(theta) <= angle / 2 && std::abs(fromAxis.y) <= p.height / 2;
		if (onArc && !front)
			return false; // the overlay's back is in the way
		if (front && (onArc || !filled)) {
			const bool inside = fill(t, fromAxis + axis, -outwards, 0.5f + theta / angle, fromAxis.y / p.height + 0.5f);
			filled = true;
			if (onArc)
				return inside;
		}
	}
	return false;
}

bool InMask(const std::vector<OOVR_VROverlayIntersectionMaskPrimitive_t>& mask, float x, float y)
{
	if (mask.empty())
		return true;
	for (const OOVR_VROverlayIntersectionMaskPrimitive_t& m : mask) {
		if (m.m_nPrimitiveType == OverlayIntersectionPrimitiveType_Rectangle) {
			const OOVR_IntersectionMaskRectangle_t& r = m.m_Primitive.m_Rectangle;
			if (x >= r.m_flTopLeftX && x <= r.m_flTopLeftX + r.m_flWidth && y >= r.m_flTopLeftY && y <= r.m_flTopLeftY + r.m_flHeight)
				return true;
		} else if (m.m_nPrimitiveType == OverlayIntersectionPrimitiveType_Circle) {
			const OOVR_IntersectionMaskCircle_t& c = m.m_Primitive.m_Circle;
			const float dx = x - c.m_flCenterX, dy = y - c.m_flCenterY;
			if (dx * dx + dy * dy <= c.m_flRadius * c.m_flRadius)
				return true;
		}
	}
	return false;
}

XrPosef ChangeOrigin(const XrPosef& pose, vr::ETrackingUniverseOrigin from, vr::ETrackingUniverseOrigin to, const XrPosef& seatedInStanding)
{
	const bool fromSeated = from == vr::TrackingUniverseSeated;
	const bool toSeated = to == vr::TrackingUniverseSeated;
	if (fromSeated == toSeated)
		return pose;
	return fromSeated ? Compose(seatedInStanding, pose) : Compose(Inverse(seatedInStanding), pose);
}

XrPosef PointOn(const Placement& p, float u, float v)
{
	const float y = (v - 0.5f) * p.height;
	if (!(p.curvature > 0))
		return Compose(p.pose, XrPosef{ { 0, 0, 0, 1 }, { (u - 0.5f) * p.width, y, 0 } });
	const float radius = CylinderRadius(p.width, p.curvature);
	const float theta = (u - 0.5f) * CylinderAngle(p.curvature);
	// On the arc, turned to face the cylinder's axis
	const XrPosef local{ X(AboutY(-theta)), { radius * std::sin(theta), y, radius - radius * std::cos(theta) } };
	return Compose(p.pose, local);
}

Placement ProjectionQuad(const XrPosef& eye, float left, float right, float top, float bottom)
{
	// GetProjectionRaw's top and bottom grow downwards; the eye looks along its -Z
	const XrPosef centre{ { 0, 0, 0, 1 }, { (left + right) / 2, -(top + bottom) / 2, -1 } };
	return Placement{ Compose(eye, centre), std::abs(right - left), std::abs(bottom - top), 0 };
}

XrPosef CursorPose(const XrPosef& point, float width, float height, const vr::HmdVector2_t& hotspot)
{
	// The hotspot from the cursor's centre, in its own space (Y up)
	const glm::vec3 hotspotOffset((hotspot.v[0] - 0.5f) * width, (0.5f - hotspot.v[1]) * height, 0);
	const glm::quat q = Q(point.orientation);
	return XrPosef{ point.orientation, X(V(point.position) - q * hotspotOffset) };
}

Chain ResolveChain(const void* overlay, const std::function<ChainLink(const void*)>& link, glm::mat4& out)
{
	std::set<const void*> seen;
	glm::mat4 withinRoot(1.0f); // the overlay in the farthest parent reached so far
	const void* node = overlay;
	for (int depth = 0; depth <= kMaxChainDepth; depth++) {
		if (!seen.insert(node).second)
			return Chain::Cycle;
		const ChainLink l = link(node);
		if (!l.relative) {
			if (!l.placed)
				return Chain::Unplaced;
			out = l.transform * withinRoot;
			return Chain::Placed;
		}
		withinRoot = l.transform * withinRoot;
		node = l.parent;
	}
	return Chain::TooDeep;
}

bool DrawnBefore(const DrawOrder& a, const DrawOrder& b)
{
	if (a.highQuality != b.highQuality)
		return b.highQuality;
	if (a.sortOrder != b.sortOrder)
		return a.sortOrder < b.sortOrder;
	return a.distance > b.distance;
}

float QuadAspect(int32_t texelsWide, int32_t texelsHigh, float texelAspect)
{
	const int32_t high = texelsHigh < 0 ? -texelsHigh : texelsHigh;
	if (high <= 0 || texelsWide <= 0)
		return 1.0f;
	const float aspect = (float)texelsWide / (float)high;
	return texelAspect > 0 && std::isfinite(texelAspect) ? aspect * texelAspect : aspect;
}

XrRect2Di ImageRect(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t width, uint32_t height, bool mirrorX, bool mirrorY)
{
	if (mirrorX) {
		const uint32_t from = width - x1, to = width - x0;
		x0 = from, x1 = to;
	}
	if (mirrorY) {
		const uint32_t from = height - y1, to = height - y0;
		y0 = from, y1 = to;
	}
	return XrRect2Di{ { (int32_t)x0, (int32_t)y0 }, { (int32_t)(x1 - x0), (int32_t)(y1 - y0) } };
}

XrColor4f ColourScale(float r, float g, float b, float a, bool premultiplied)
{
	auto clamp = [](float v) { return v >= 0 ? (v <= 1 ? v : 1.0f) : 0.0f; }; // NaN gives 0
	a = clamp(a);
	const float fade = premultiplied ? a : 1.0f;
	return XrColor4f{ clamp(r) * fade, clamp(g) * fade, clamp(b) * fade, a };
}

float SubtendedAngle(float width, float distance)
{
	if (!(width > 0) || !(distance >= 0))
		return 0;
	return 2 * std::atan2(width / 2, distance);
}

XrPosef ComposePoses(const XrPosef& a, const XrPosef& b)
{
	return Compose(a, b);
}

XrPosef InversePose(const XrPosef& a)
{
	return Inverse(a);
}

} // namespace overlay_maths
