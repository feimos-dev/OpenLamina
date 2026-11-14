#include "stdafx.h"

#include "Foveation.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace Foveation {

std::optional<vr::HmdVector2_t> CenterForProjection(const glm::vec3& direction, const vr::HmdMatrix44_t& projection)
{
	float length = glm::length(direction);
	if (!(length > 0))
		return std::nullopt;
	glm::vec3 d = direction / length;

	// The matrix (row-major) times (d, 0): a point at infinity in that direction
	const auto& m = projection.m;
	float x = m[0][0] * d.x + m[0][1] * d.y + m[0][2] * d.z;
	float y = m[1][0] * d.x + m[1][1] * d.y + m[1][2] * d.z;
	float w = m[3][0] * d.x + m[3][1] * d.y + m[3][2] * d.z;

	// Behind the view, or (nearly) along its plane: nowhere on it
	if (!(w > 1e-4f) || !std::isfinite(x) || !std::isfinite(y))
		return std::nullopt;

	vr::HmdVector2_t ndc;
	ndc.v[0] = std::clamp(x / w, -1.0f, 1.0f);
	ndc.v[1] = std::clamp(y / w, -1.0f, 1.0f);
	return ndc;
}

int EyeOfProjection(const vr::HmdMatrix44_t& projection, const vr::HmdMatrix44_t& left, const vr::HmdMatrix44_t& right)
{
	// Y's terms with the sign of a flipped Y undone
	auto terms = [](const vr::HmdMatrix44_t& m) {
		float sign = m.m[1][1] < 0 ? -1.0f : 1.0f;
		return std::array<float, 4>{ m.m[0][0], m.m[0][2], sign * m.m[1][1], sign * m.m[1][2] };
	};
	auto distance = [&](const vr::HmdMatrix44_t& eye) {
		std::array<float, 4> a = terms(projection), b = terms(eye);
		float sum = 0;
		for (size_t i = 0; i < a.size(); i++)
			sum += std::fabs(a[i] - b[i]);
		return sum;
	};
	float toLeft = distance(left), toRight = distance(right);
	if (std::fabs(toLeft - toRight) <= 1e-4f)
		return -1;
	return toLeft < toRight ? 0 : 1;
}

glm::vec3 GazeInEye(const glm::quat& gazeInHead, const glm::quat& eyeInHead)
{
	return glm::inverse(eyeInHead) * (gazeInHead * glm::vec3(0, 0, -1));
}

} // namespace Foveation
