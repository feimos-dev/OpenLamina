#include "stdafx.h"

#include "Bounds.h"

#include <algorithm>
#include <cmath>

float BoundsAlpha(float distanceToEdge)
{
	if (!(distanceToEdge > 0.0f))
		return 1.0f; // at the edge or beyond (or no distance at all)
	if (distanceToEdge >= kBoundsNear)
		return 0.0f;
	return 1.0f - distanceToEdge / kBoundsNear;
}

std::vector<BoundsWall> BoundsWalls(float width, float depth, const std::vector<XrVector3f>& points)
{
	std::vector<BoundsWall> walls;
	if (!(width > 0.0f) || !(depth > 0.0f))
		return walls;
	const float hw = width / 2, hd = depth / 2;
	const float s = std::sqrt(0.5f);
	// Each edge: where its wall stands, the turn about +Y that faces a quad's +Z into the play area, and its width
	struct Edge {
		XrVector3f centre;
		XrQuaternionf facing;
		float wallWidth;
	};
	const Edge edges[4] = {
		{ { hw, kBoundsHeight / 2, 0 }, { 0, -s, 0, s }, depth }, // +X, facing -X
		{ { -hw, kBoundsHeight / 2, 0 }, { 0, s, 0, s }, depth }, // -X, facing +X
		{ { 0, kBoundsHeight / 2, hd }, { 0, 1, 0, 0 }, width }, // +Z, facing -Z
		{ { 0, kBoundsHeight / 2, -hd }, { 0, 0, 0, 1 }, width }, // -Z, facing +Z
	};
	for (int e = 0; e < 4; e++) {
		float alpha = 0.0f;
		for (const XrVector3f& p : points) {
			const float distance = e == 0 ? hw - p.x : e == 1 ? hw + p.x : e == 2 ? hd - p.z : hd + p.z;
			alpha = std::max(alpha, BoundsAlpha(distance));
		}
		if (alpha > 0.0f)
			walls.push_back(BoundsWall{ e, XrPosef{ edges[e].facing, edges[e].centre }, edges[e].wallWidth, kBoundsHeight, alpha });
	}
	return walls;
}

void DrawBoundsGrid(Canvas& canvas, float alpha)
{
	const uint32_t a = (uint32_t)std::lround(std::clamp(alpha, 0.0f, 1.0f) * 220.0f);
	canvas.Clear(0x00000000);
	const uint32_t line = 0x40c8ff00u | a;
	const int w = (int)canvas.Pixels().width, h = (int)canvas.Pixels().height;
	constexpr int kCell = 32, kLine = 3;
	for (int x = 0; x < w; x += kCell)
		canvas.FillRect(x, 0, kLine, h, line);
	for (int y = 0; y < h; y += kCell)
		canvas.FillRect(0, y, w, kLine, line);
	// The last lines at the far edges
	canvas.FillRect(w - kLine, 0, kLine, h, line);
	canvas.FillRect(0, h - kLine, w, kLine, line);
}
