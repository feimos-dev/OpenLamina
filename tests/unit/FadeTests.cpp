// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "Misc/Fade.h"

#include <cmath>

TEST_CASE("Skybox_textures_need_one_two_or_six_with_handles")
{
	int a = 0;
	vr::Texture_t t[7];
	for (auto& x : t)
		x = { &a, vr::TextureType_OpenGL, vr::ColorSpace_Auto };
	CHECK(SkyboxTexturesValid(t, 1));
	CHECK(SkyboxTexturesValid(t, 2));
	CHECK(SkyboxTexturesValid(t, 6));
	CHECK_FALSE(SkyboxTexturesValid(t, 0));
	CHECK_FALSE(SkyboxTexturesValid(t, 3));
	CHECK_FALSE(SkyboxTexturesValid(t, 4));
	CHECK_FALSE(SkyboxTexturesValid(t, 5));
	CHECK_FALSE(SkyboxTexturesValid(t, 7));
	CHECK_FALSE(SkyboxTexturesValid(nullptr, 1));
	t[1].handle = nullptr;
	CHECK(SkyboxTexturesValid(t, 1));
	CHECK_FALSE(SkyboxTexturesValid(t, 2));
}

TEST_CASE("Skybox_request_keeps_its_own_copy_of_the_textures")
{
	vr::VRVulkanTextureData_t data{};
	data.m_nWidth = 640;
	data.m_nHeight = 480;
	int gl = 3;
	vr::Texture_t t[2] = { { &data, vr::TextureType_Vulkan, vr::ColorSpace_Auto }, { &gl, vr::TextureType_OpenGL, vr::ColorSpace_Auto } };
	SkyboxRequest request;
	REQUIRE(request.Set(t, 2));
	CHECK(request.Count() == 2);
	data.m_nWidth = 1; // the program's struct goes away or changes
	const auto* kept = (const vr::VRVulkanTextureData_t*)request.Textures()[0].handle;
	CHECK(kept != &data);
	CHECK(kept->m_nWidth == 640);
	CHECK(request.Textures()[1].handle == &gl);

	SkyboxRequest copy = request;
	CHECK(copy.Textures()[0].handle != request.Textures()[0].handle);
	CHECK(((const vr::VRVulkanTextureData_t*)copy.Textures()[0].handle)->m_nHeight == 480);

	request.Clear();
	CHECK(request.Empty());
	CHECK_FALSE(copy.Empty());
	CHECK_FALSE(request.Set(t, 3));
}

TEST_CASE("Ramp_moves_linearly_from_where_it_is_when_told")
{
	Ramp<float> r(0.0f);
	CHECK(r.At(5) == 0.0f);
	r.Start(10, 2, 1.0f);
	CHECK(r.At(10) == doctest::Approx(0.0f));
	CHECK(r.At(11) == doctest::Approx(0.5f));
	CHECK(r.At(12) == doctest::Approx(1.0f));
	CHECK(r.At(50) == doctest::Approx(1.0f));
	// A new target part-way starts from the value at that moment
	r.Start(11, 1, 0.0f);
	CHECK(r.At(11) == doctest::Approx(0.5f));
	CHECK(r.At(11.5) == doctest::Approx(0.25f));
	CHECK(r.At(12) == doctest::Approx(0.0f));
	// No time, or a time that isn't one, is at once
	r.Start(20, 0, 0.75f);
	CHECK(r.At(20) == doctest::Approx(0.75f));
	r.Start(30, -1, 0.25f);
	CHECK(r.At(30) == doctest::Approx(0.25f));
	r.Start(40, NAN, 1.0f);
	CHECK(r.At(40) == doctest::Approx(1.0f));
}

TEST_CASE("Fades_scene_and_background_are_separate_and_start_clear")
{
	CompositorFades f;
	vr::HmdColor_t c = f.ColourAt(100, false);
	CHECK(c.a == 0.0f);
	CHECK_FALSE(FadeVisible(c));

	f.FadeToColour(100, 4, { 1, 0.5f, 0, 1 }, false);
	c = f.ColourAt(100, false);
	CHECK(c.a == doctest::Approx(0.0f));
	c = f.ColourAt(102, false);
	CHECK(c.r == doctest::Approx(0.5f));
	CHECK(c.g == doctest::Approx(0.25f));
	CHECK(c.a == doctest::Approx(0.5f));
	CHECK(FadeVisible(c));
	c = f.ColourAt(104, false);
	CHECK(c.r == doctest::Approx(1.0f));
	CHECK(c.a == doctest::Approx(1.0f));
	// The background is untouched
	CHECK(f.ColourAt(104, true).a == 0.0f);

	f.FadeToColour(104, 0, { 0, 0, 0, 0 }, false);
	CHECK_FALSE(FadeVisible(f.ColourAt(104, false)));
}

TEST_CASE("Grid_alpha_follows_the_time_asked")
{
	CompositorFades f;
	CHECK(f.GridAlphaAt(0) == 0.0f);
	CHECK_FALSE(f.GridWanted());
	f.FadeGrid(10, 2, true);
	CHECK(f.GridWanted());
	CHECK(f.GridAlphaAt(10) == doctest::Approx(0.0f));
	CHECK(f.GridAlphaAt(11) == doctest::Approx(0.5f));
	CHECK(f.GridAlphaAt(12) == doctest::Approx(1.0f));
	f.FadeGrid(12, 4, false);
	CHECK_FALSE(f.GridWanted());
	CHECK(f.GridAlphaAt(14) == doctest::Approx(0.5f));
	CHECK(f.GridAlphaAt(16) == doctest::Approx(0.0f));
	f.Reset();
	CHECK(f.GridAlphaAt(12) == 0.0f);
}

TEST_CASE("The_skybox_replaces_the_scene_only_when_the_grid_is_fully_in")
{
	ScenePlan p = PlanScene(true, true, 0.0f);
	CHECK(p.scene);
	CHECK_FALSE(p.skybox);
	p = PlanScene(true, true, 0.99f);
	CHECK(p.scene);
	CHECK_FALSE(p.skybox);
	p = PlanScene(true, true, 1.0f);
	CHECK_FALSE(p.scene);
	CHECK(p.skybox);
	// Without a skybox the scene stays
	p = PlanScene(true, false, 1.0f);
	CHECK(p.scene);
	CHECK_FALSE(p.skybox);
	// With no scene, the skybox shows only while the grid is in
	p = PlanScene(false, true, 0.0f);
	CHECK_FALSE(p.scene);
	CHECK_FALSE(p.skybox);
}

TEST_CASE("Fade_texels_are_rounded_and_clamped")
{
	uint8_t t[4];
	FadeTexel({ 0, 0.5f, 1, 0.25f }, t);
	CHECK(t[0] == 0);
	CHECK(t[1] == 128);
	CHECK(t[2] == 255);
	CHECK(t[3] == 64);
	FadeTexel({ -1, 2, NAN, 1 }, t);
	CHECK(t[0] == 0);
	CHECK(t[1] == 255);
	CHECK(t[2] == 0);
	CHECK(t[3] == 255);
}
