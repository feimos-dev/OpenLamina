#pragma once
// Overlay placement and intersection: where an overlay is drawn, and where a ray meets it

#include "generated/interfaces/vrtypes.h"

#include <glm/mat4x4.hpp>
#include <openxr/openxr.h>

#include <functional>
#include <vector>

// IVROverlay's intersection mask primitives (SetOverlayIntersectionMask)
enum OOVR_EVROverlayIntersectionMaskPrimitiveType {
	OverlayIntersectionPrimitiveType_Rectangle,
	OverlayIntersectionPrimitiveType_Circle,
};

struct OOVR_IntersectionMaskRectangle_t {
	float m_flTopLeftX;
	float m_flTopLeftY;
	float m_flWidth;
	float m_flHeight;
};

struct OOVR_IntersectionMaskCircle_t {
	float m_flCenterX;
	float m_flCenterY;
	float m_flRadius;
};

typedef union {
	OOVR_IntersectionMaskRectangle_t m_Rectangle;
	OOVR_IntersectionMaskCircle_t m_Circle;
} OOVR_VROverlayIntersectionMaskPrimitive_Data_t;

struct OOVR_VROverlayIntersectionMaskPrimitive_t {
	OOVR_EVROverlayIntersectionMaskPrimitiveType m_nPrimitiveType;
	OOVR_VROverlayIntersectionMaskPrimitive_Data_t m_Primitive;
};

namespace overlay_maths {

// An overlay as drawn, in the frame's reference space. The pose is the overlay's centre, facing +Z (it is seen from
// +Z); a curved overlay bends towards +Z, around a cylinder whose axis is its Y axis, a radius in front of it.
// Curvature as OpenVR's: the width over the cylinder's circumference, 0 flat, 1 a closed cylinder.
struct Placement {
	XrPosef pose;
	float width, height;
	float curvature;
};

// Where a ray meets an overlay. u, v are 0..1 across it, (0, 0) its lower left; the normal faces the ray's origin.
struct Hit {
	float distance, u, v;
	XrVector3f point, normal;
};

// The cylinder a curved overlay is drawn on: its radius, and the angle the overlay covers (below 2π)
float CylinderRadius(float width, float curvature);
float CylinderAngle(float curvature);

// Intersects a ray (dir needn't be unit length) with the overlay's front, a quad or (curvature > 0) a cylinder's
// inside. hit is filled wherever the ray meets the overlay's plane or cylinder from the front, even off the overlay;
// returns true only on the overlay itself.
bool Intersect(const Placement& p, const XrVector3f& origin, const XrVector3f& dir, Hit& hit);

// Whether (x, y) is in any of the mask's primitives (in the mask's own units); an empty mask is the whole overlay
bool InMask(const std::vector<OOVR_VROverlayIntersectionMaskPrimitive_t>& mask, float x, float y);

// A pose in one tracking origin, in another. seatedInStanding is the seated origin in the standing one; the raw
// origin is the standing one.
XrPosef ChangeOrigin(const XrPosef& pose, vr::ETrackingUniverseOrigin from, vr::ETrackingUniverseOrigin to, const XrPosef& seatedInStanding);

// The point at (u, v) on the overlay ((0, 0) its lower left), facing out of the overlay there
XrPosef PointOn(const Placement& p, float u, float v);

// A projection overlay: the quad 1 m ahead of the eye that covers the eye's projection. The tangents are as
// IVRSystem::GetProjectionRaw gives them (top above the centre is negative).
Placement ProjectionQuad(const XrPosef& eye, float left, float right, float top, float bottom);

// A cursor overlay at a point: its centre, so that its hotspot (texture coordinates, (0, 0) the upper left) is on the
// point
XrPosef CursorPose(const XrPosef& point, float width, float height, const vr::HmdVector2_t& hotspot);

// Overlay-relative chains: an overlay placed relative to another, which may itself be relative to another, and so on
constexpr int kMaxChainDepth = 8;

enum class Chain {
	Placed, // out is the overlay's transform
	Unplaced, // the chain ends at an overlay that can't be placed (no such overlay, or an untracked device)
	Cycle, // the chain comes back to an overlay already in it
	TooDeep, // more than kMaxChainDepth overlay-relative links
};

// What one overlay is placed by: its parent and its transform within it, or (not relative) its own transform
struct ChainLink {
	bool relative = false;
	const void* parent = nullptr;
	glm::mat4 transform{ 1.0f };
	bool placed = false; // not relative: whether transform holds the overlay's own placement
};

// Resolves an overlay's transform through its parents, the farthest first; transforms are ordinary glm matrices
Chain ResolveChain(const void* overlay, const std::function<ChainLink(const void*)>& link, glm::mat4& out);

// What decides the order overlays are drawn in: the high quality overlay on top of the rest, then by sort order, then
// the farthest first
struct DrawOrder {
	bool highQuality;
	uint32_t sortOrder;
	float distance;
};
// Whether a is drawn before (under) b
bool DrawnBefore(const DrawOrder& a, const DrawOrder& b);

// A quad's width over its height from its texture's size in texels. A flipped texture has a negative height. A texel
// aspect of 2 means texels twice as wide as tall; a size or aspect that makes no sense gives 1.
float QuadAspect(int32_t texelsWide, int32_t texelsHigh, float texelAspect);

// The scale of an overlay's layer for its colour and alpha, each kept between 0 and 1. A premultiplied texture's colour
// is faded with its alpha too.
XrColor4f ColourScale(float r, float g, float b, float a, bool premultiplied);

// The angle in radians a quad of this width covers, seen from straight ahead at this distance
float SubtendedAngle(float width, float distance);

// a then b (b given in a's space), and a's inverse
XrPosef ComposePoses(const XrPosef& a, const XrPosef& b);
XrPosef InversePose(const XrPosef& a);

} // namespace overlay_maths
