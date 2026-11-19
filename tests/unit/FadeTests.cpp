// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "Misc/Fade.h"

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
