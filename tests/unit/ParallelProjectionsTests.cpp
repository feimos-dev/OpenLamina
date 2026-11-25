#include "doctest.h"

#include "stdafx.h"

#include "Misc/ParallelProjections.h"
#include "Misc/xrmoreutils.h"
#include "convert.h"

#include <cmath>
#include <glm/trigonometric.hpp>

using parallel_projections::Choice;

static XrQuaternionf Yaw(float degrees)
{
	const float half = glm::radians(degrees) / 2;
	return { 0, sinf(half), 0, cosf(half) };
}

TEST_CASE("A parallel view looks where the head does and takes in all of the turned view")
{
	const float quarter = glm::radians(45.0f);
	const XrFovf square{ -quarter, quarter, quarter, -quarter };
	XrView view{ XR_TYPE_VIEW };
	view.fov = square;
	view.pose = { Yaw(10.0f), { -0.03f, 0, 0 } };
	CHECK(xr_utils::ParallelView(view, Yaw(10.0f)));
	CHECK(glm::degrees(view.fov.angleLeft) == doctest::Approx(-55.0f).epsilon(0.001));
	CHECK(glm::degrees(view.fov.angleRight) == doctest::Approx(35.0f).epsilon(0.001));
	// Taller where the turned view's near edge comes closer
	const float up = glm::degrees(atanf(1.0f / (cosf(glm::radians(10.0f)) - sinf(glm::radians(10.0f)))));
	CHECK(glm::degrees(view.fov.angleUp) == doctest::Approx(up).epsilon(0.001));
	CHECK(view.fov.angleDown == doctest::Approx(-view.fov.angleUp));
	CHECK(view.pose.orientation.y == doctest::Approx(0.0f));
	CHECK(view.pose.orientation.w == doctest::Approx(1.0f));
	CHECK(view.pose.position.x == -0.03f);

	// In a turned head: the head's orientation
	XrView turned{ XR_TYPE_VIEW };
	turned.fov = square;
	turned.pose.orientation = G2X_quat(X2G_quat(Yaw(30.0f)) * X2G_quat(Yaw(-5.0f)));
	xr_utils::ParallelView(turned, Yaw(-5.0f));
	CHECK(turned.pose.orientation.y == doctest::Approx(Yaw(30.0f).y));
	CHECK(turned.pose.orientation.w == doctest::Approx(Yaw(30.0f).w));
	CHECK(glm::degrees(turned.fov.angleRight) == doctest::Approx(50.0f).epsilon(0.001));

	// Eyes that aren't turned keep their view exactly
	XrView straight{ XR_TYPE_VIEW };
	straight.fov = square;
	straight.pose.orientation = { 0, 0, 0, 1 };
	CHECK(xr_utils::ParallelView(straight, { 0, 0, 0, 1 }));
	CHECK(straight.fov.angleLeft == -quarter);
	CHECK(straight.fov.angleUp == quarter);

	// A view turned so far that part of it is behind the head stays
	XrView behind{ XR_TYPE_VIEW };
	behind.fov = square;
	behind.pose.orientation = Yaw(60.0f);
	CHECK_FALSE(xr_utils::ParallelView(behind, Yaw(60.0f)));
	CHECK(behind.fov.angleLeft == -quarter);
	CHECK(behind.pose.orientation.y == Yaw(60.0f).y);
}

TEST_CASE("A point on a turned view lands where the head sees it")
{
	XrVector2f centre = xr_utils::ParallelTangents({ 0, 0 }, Yaw(10.0f));
	CHECK(centre.x == doctest::Approx(-tanf(glm::radians(10.0f))));
	CHECK(centre.y == doctest::Approx(0.0f));
	XrVector2f same = xr_utils::ParallelTangents({ 0.5f, -0.25f }, { 0, 0, 0, 1 });
	CHECK(same.x == doctest::Approx(0.5f));
	CHECK(same.y == doctest::Approx(-0.25f));
}

TEST_CASE("The image grows with the widened view")
{
	const float quarter = glm::radians(45.0f);
	const XrFovf square{ -quarter, quarter, quarter, -quarter };
	XrView view{ XR_TYPE_VIEW };
	view.fov = square;
	view.pose.orientation = Yaw(10.0f);
	xr_utils::ParallelView(view, Yaw(10.0f));
	glm::vec2 scale = xr_utils::TangentSpanScale(square, view.fov);
	CHECK(scale.x == doctest::Approx((tanf(glm::radians(35.0f)) + tanf(glm::radians(55.0f))) / 2.0f).epsilon(0.001));
	CHECK(scale.y > 1.0f);
	CHECK(xr_utils::TangentSpanScale(square, square).x == doctest::Approx(1.0f));
	// A view with no size gives no scale
	CHECK(xr_utils::TangentSpanScale(XrFovf{}, square).x == 1.0f);
}

TEST_CASE("Known games are found by their executable's file name, whatever its case")
{
	CHECK(parallel_projections::KnownGame("A:\\SteamLibrary\\steamapps\\common\\Elite Dangerous\\Products\\elite-dangerous-odyssey-64\\EliteDangerous64.exe") == "Elite Dangerous");
	CHECK(parallel_projections::KnownGame("elitedangerous64.EXE") == "Elite Dangerous");
	CHECK(parallel_projections::KnownGame("/home/user/games/AMS2AVX.exe") == "Automobilista 2");
	CHECK(parallel_projections::KnownGame("C:\\Games\\EliteDangerous64.exe.bak").empty());
	CHECK(parallel_projections::KnownGame("C:\\Games\\hlvr.exe").empty());
	CHECK(parallel_projections::KnownGame("").empty());
}

TEST_CASE("The game list's choice wins, then openlamina.ini's, then the known games")
{
	const char* elite = "C:\\Elite\\EliteDangerous64.exe";
	const char* other = "C:\\Games\\hlvr.exe";
	CHECK(parallel_projections::Decide(Choice::Automatic, Choice::Automatic, elite).on);
	CHECK_FALSE(parallel_projections::Decide(Choice::Automatic, Choice::Automatic, other).on);
	CHECK_FALSE(parallel_projections::Decide(Choice::Automatic, Choice::Off, elite).on);
	CHECK(parallel_projections::Decide(Choice::Automatic, Choice::On, other).on);
	CHECK(parallel_projections::Decide(Choice::On, Choice::Off, other).on);
	CHECK_FALSE(parallel_projections::Decide(Choice::Off, Choice::On, elite).on);
	CHECK(parallel_projections::Decide(Choice::Automatic, Choice::Automatic, elite).reason.find("Elite Dangerous") != std::string::npos);
}
