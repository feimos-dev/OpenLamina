#pragma once
// The play area's bounds (drawBounds): a grid wall on each edge of the play area that the HMD or a controller comes
// within kBoundsNear of, fading in as it comes closer. The play area is the stage's bounds rectangle
// (xrGetReferenceSpaceBoundsRect), else the committed room setup's play area, centred on the stage's origin.

#include "Canvas.h"
#include "json/json.h"

#include <openxr/openxr.h>

#include <vector>

constexpr float kBoundsNear = 0.4f; // metres from the edge at which a wall starts to show
constexpr float kBoundsHeight = 2.5f; // the walls' height, from the floor

// 1 at 0 m from the edge (or beyond it), 0 at kBoundsNear and further, linear in between
float BoundsAlpha(float distanceToEdge);

struct BoundsWall {
	int edge; // 0 the +X edge, 1 the -X one, 2 +Z, 3 -Z
	XrPosef pose; // in the stage, facing into the play area (a quad's +Z)
	float width, height;
	float alpha;
};

// The play area: runtime (the stage's bounds) where the runtime knows them (not null), else the play area of the
// committed room setup (the store's Chaperone document), else 0 by 0
XrExtent2Df BoundsPlayArea(const XrExtent2Df* runtime, const Json::Value& chaperoneDoc);

// The play area OpenLamina answers with everywhere (IVRChaperone's getters, GetCalibrationState and the drawn
// bounds): BoundsPlayArea of the OpenXR runtime's stage bounds (asked of it once per session and after the stage
// changes) and the store's committed room setup. 0 by 0 without either.
XrExtent2Df CurrentPlayArea();

// The walls to draw for a play area width (X) by depth (Z), given the tracked points in the stage. With preview (a
// room setup tool's ShowWorkingSetPreview), every wall, whole, wherever the points are.
std::vector<BoundsWall> BoundsWalls(float width, float depth, const std::vector<XrVector3f>& points, bool preview = false);

// A wall's grid, alpha times as opaque as it is at the edge
void DrawBoundsGrid(Canvas& canvas, float alpha);
