// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

#include "Misc/Foveation.h"

#include <cmath>

namespace {

const float kRad = 3.14159265f / 180.0f;

// An OpenVR projection (row-major) from the view's tangents, OpenGL's depth range
vr::HmdMatrix44_t Projection(float tanLeft, float tanRight, float tanDown, float tanUp, float zNear = 0.1f, float zFar = 100.0f)
{
	float w = tanRight - tanLeft, h = tanUp - tanDown;
	vr::HmdMatrix44_t m = {};
	m.m[0][0] = 2 / w;
	m.m[0][2] = (tanRight + tanLeft) / w;
	m.m[1][1] = 2 / h;
	m.m[1][2] = (tanUp + tanDown) / h;
	m.m[2][2] = -(zFar + zNear) / (zFar - zNear);
	m.m[2][3] = -2 * zFar * zNear / (zFar - zNear);
	m.m[3][2] = -1;
	return m;
}

// DirectX's depth range, the same x and y
vr::HmdMatrix44_t ProjectionDX(float tanLeft, float tanRight, float tanDown, float tanUp, float zNear = 0.1f, float zFar = 100.0f)
{
	vr::HmdMatrix44_t m = Projection(tanLeft, tanRight, tanDown, tanUp, zNear, zFar);
	m.m[2][2] = -zFar / (zFar - zNear);
	m.m[2][3] = -zFar * zNear / (zFar - zNear);
	return m;
}

// Y flipped, as a Vulkan game's own matrix may be
vr::HmdMatrix44_t FlipY(vr::HmdMatrix44_t m)
{
	for (float& v : m.m[1])
		v = -v;
	return m;
}

// Looking yaw degrees right and pitch degrees up
glm::vec3 Direction(float yawDeg, float pitchDeg)
{
	float yaw = yawDeg * kRad, pitch = pitchDeg * kRad;
	return { std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch) };
}

glm::quat Turn(float yawDeg, float pitchDeg) // the rotation that turns -Z into Direction(yaw, pitch)
{
	return glm::angleAxis(-yawDeg * kRad, glm::vec3(0, 1, 0)) * glm::angleAxis(pitchDeg * kRad, glm::vec3(1, 0, 0));
}

} // namespace

TEST_CASE("Foveation: straight ahead is the centre of a symmetric view")
{
	auto c = Foveation::CenterForProjection({ 0, 0, -1 }, Projection(-1, 1, -1, 1));
	REQUIRE(c);
	CHECK(c->v[0] == doctest::Approx(0));
	CHECK(c->v[1] == doctest::Approx(0));
}

TEST_CASE("Foveation: an asymmetric view's centre is where its tangents put the gaze")
{
	// The left eye of a headset: more to the left than to the right, more down than up
	float l = std::tan(-50 * kRad), r = std::tan(40 * kRad), d = std::tan(-48 * kRad), u = std::tan(42 * kRad);
	vr::HmdMatrix44_t p = Projection(l, r, d, u);
	glm::vec3 dir = Direction(10, -5);
	float tx = dir.x / -dir.z, ty = dir.y / -dir.z;
	auto c = Foveation::CenterForProjection(dir, p);
	REQUIRE(c);
	CHECK(c->v[0] == doctest::Approx((2 * tx - l - r) / (r - l)));
	CHECK(c->v[1] == doctest::Approx((2 * ty - d - u) / (u - d)));

	// Straight ahead is off the centre of such a view
	auto ahead = Foveation::CenterForProjection({ 0, 0, -1 }, p);
	REQUIRE(ahead);
	CHECK(ahead->v[0] == doctest::Approx(-(l + r) / (r - l)));
}

TEST_CASE("Foveation: the depth range and the graphics convention don't move the centre, a flipped Y flips it")
{
	glm::vec3 dir = Direction(-12, 8);
	auto gl = Foveation::CenterForProjection(dir, Projection(-1.2f, 1.0f, -1.1f, 0.9f));
	auto glFar = Foveation::CenterForProjection(dir, Projection(-1.2f, 1.0f, -1.1f, 0.9f, 0.01f, 10000.0f));
	auto dx = Foveation::CenterForProjection(dir, ProjectionDX(-1.2f, 1.0f, -1.1f, 0.9f));
	auto flipped = Foveation::CenterForProjection(dir, FlipY(Projection(-1.2f, 1.0f, -1.1f, 0.9f)));
	REQUIRE((gl && glFar && dx && flipped));
	CHECK(gl->v[0] < 0); // left
	CHECK(gl->v[1] > 0); // up
	CHECK(glFar->v[0] == doctest::Approx(gl->v[0]));
	CHECK(glFar->v[1] == doctest::Approx(gl->v[1]));
	CHECK(dx->v[0] == doctest::Approx(gl->v[0]));
	CHECK(dx->v[1] == doctest::Approx(gl->v[1]));
	CHECK(flipped->v[0] == doctest::Approx(gl->v[0]));
	CHECK(flipped->v[1] == doctest::Approx(-gl->v[1]));
}

TEST_CASE("Foveation: a gaze past the view's edge gives the edge, one behind gives nothing")
{
	vr::HmdMatrix44_t p = Projection(-1, 1, -1, 1); // 45 degrees each way
	auto past = Foveation::CenterForProjection(Direction(60, 0), p);
	REQUIRE(past);
	CHECK(past->v[0] == doctest::Approx(1));
	CHECK(past->v[1] == doctest::Approx(0));
	CHECK_FALSE(Foveation::CenterForProjection({ 0, 0, 1 }, p));
	CHECK_FALSE(Foveation::CenterForProjection({ 1, 0, 0 }, p));
	CHECK_FALSE(Foveation::CenterForProjection(Direction(100, 0), p));
	p.m[0][2] = NAN;
	CHECK_FALSE(Foveation::CenterForProjection({ 0, 0, -1 }, p));
}

TEST_CASE("Foveation: a projection is matched to its eye")
{
	vr::HmdMatrix44_t left = Projection(-1.2f, 1.0f, -1.1f, 0.9f), right = Projection(-1.0f, 1.2f, -1.1f, 0.9f);
	CHECK(Foveation::EyeOfProjection(left, left, right) == 0);
	CHECK(Foveation::EyeOfProjection(right, left, right) == 1);
	// The game's own depth range, DirectX's, or Y flipped: still the same eye
	CHECK(Foveation::EyeOfProjection(Projection(-1.0f, 1.2f, -1.1f, 0.9f, 0.05f, 500), left, right) == 1);
	CHECK(Foveation::EyeOfProjection(ProjectionDX(-1.2f, 1.0f, -1.1f, 0.9f), left, right) == 0);
	CHECK(Foveation::EyeOfProjection(FlipY(right), left, right) == 1);
	// Jittered by a few pixels for TAA, or a game's own slightly wider view: the nearer eye
	vr::HmdMatrix44_t jittered = left;
	jittered.m[0][2] += 0.004f;
	jittered.m[1][2] -= 0.004f;
	CHECK(Foveation::EyeOfProjection(jittered, left, right) == 0);
	CHECK(Foveation::EyeOfProjection(Projection(-1.0f, 1.3f, -1.2f, 1.0f), left, right) == 1);
	// A view centred between the eyes (a narrower one of the game's own), and two eyes with the same projection: neither
	CHECK(Foveation::EyeOfProjection(Projection(-0.5f, 0.5f, -0.5f, 0.5f), left, right) == -1);
	CHECK(Foveation::EyeOfProjection(left, left, left) == -1);
}

TEST_CASE("Foveation: the gaze in an eye's frame")
{
	// Straight ahead, eyes not turned: straight ahead
	glm::vec3 d = Foveation::GazeInEye(glm::quat(1, 0, 0, 0), glm::quat(1, 0, 0, 0));
	CHECK(d.x == doctest::Approx(0));
	CHECK(d.y == doctest::Approx(0));
	CHECK(d.z == doctest::Approx(-1));

	// A gaze 10 degrees right and 5 up
	d = Foveation::GazeInEye(Turn(10, 5), glm::quat(1, 0, 0, 0));
	glm::vec3 expected = Direction(10, 5);
	CHECK(d.x == doctest::Approx(expected.x));
	CHECK(d.y == doctest::Approx(expected.y));
	CHECK(d.z == doctest::Approx(expected.z));

	// Straight ahead seen from a left eye turned 10 degrees outwards (to the left): 10 degrees right of its centre
	d = Foveation::GazeInEye(glm::quat(1, 0, 0, 0), glm::angleAxis(10 * kRad, glm::vec3(0, 1, 0)));
	expected = Direction(10, 0);
	CHECK(d.x == doctest::Approx(expected.x));
	CHECK(d.y == doctest::Approx(expected.y));
	CHECK(d.z == doctest::Approx(expected.z));
}
