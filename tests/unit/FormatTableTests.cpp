// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

#include "Compositor/FormatTable.h"

namespace {

void CheckChoice(std::optional<FormatChoice> got, int64_t format, bool convert)
{
	REQUIRE(got.has_value());
	CHECK(got->swapchainFormat == format);
	CHECK(got->convert == convert);
}

} // namespace

TEST_CASE("Same_format_offered_is_kept")
{
	// DXGI_FORMAT_R8G8B8A8_UNORM
	CheckChoice(ChooseSwapchainFormat(GfxApiKind::D3D11, 28, vr::ColorSpace_Auto, { 28 }), 28, false);
	CheckChoice(ChooseSwapchainFormat(GfxApiKind::D3D12, 28, vr::ColorSpace_Auto, { 28 }), 28, false);
}

TEST_CASE("Typeless_with_gamma_picks_srgb")
{
	// R8G8B8A8_TYPELESS under Gamma: R8G8B8A8_UNORM_SRGB, though UNORM is offered too
	CheckChoice(ChooseSwapchainFormat(GfxApiKind::D3D11, 27, vr::ColorSpace_Gamma, { 29, 28 }), 29, false);
	// Under Linear: UNORM, whatever the runtime's order
	CheckChoice(ChooseSwapchainFormat(GfxApiKind::D3D11, 27, vr::ColorSpace_Linear, { 29, 28 }), 28, false);
	// Auto at 8 bits is gamma
	CheckChoice(ChooseSwapchainFormat(GfxApiKind::D3D11, 27, vr::ColorSpace_Auto, { 28, 29 }), 29, false);
	// The other member of the family when the wanted one isn't offered: still a plain copy
	CheckChoice(ChooseSwapchainFormat(GfxApiKind::D3D11, 29, vr::ColorSpace_Gamma, { 28 }), 28, false);
}

TEST_CASE("Float_to_rgba8_converts")
{
	// R16G16B16A16_FLOAT with only R8G8B8A8_UNORM_SRGB
	CheckChoice(ChooseSwapchainFormat(GfxApiKind::D3D11, 10, vr::ColorSpace_Auto, { 29 }), 29, true);
	// Offered: kept
	CheckChoice(ChooseSwapchainFormat(GfxApiKind::D3D11, 10, vr::ColorSpace_Auto, { 29, 10 }), 10, false);
	// Typeless R16G16B16A16 is float
	CheckChoice(ChooseSwapchainFormat(GfxApiKind::D3D12, 9, vr::ColorSpace_Auto, { 10, 29 }), 10, false);
}

TEST_CASE("R11G11B10_to_rgba8_converts")
{
	CheckChoice(ChooseSwapchainFormat(GfxApiKind::D3D11, 26, vr::ColorSpace_Auto, { 29 }), 29, true);
	// Its own bit depth class first: R10G10B10A2_UNORM before RGBA8
	CheckChoice(ChooseSwapchainFormat(GfxApiKind::D3D11, 26, vr::ColorSpace_Auto, { 29, 24 }), 24, true);
}

TEST_CASE("Bgrx_to_rgba8_converts")
{
	// B8G8R8X8_UNORM, same class (8 bits) but another channel order
	CheckChoice(ChooseSwapchainFormat(GfxApiKind::D3D11, 88, vr::ColorSpace_Auto, { 29 }), 29, true);
	// B8G8R8A8_UNORM under Auto: its sRGB member, offered
	CheckChoice(ChooseSwapchainFormat(GfxApiKind::D3D11, 87, vr::ColorSpace_Auto, { 10, 91, 29 }), 91, false);
}

TEST_CASE("Vulkan_sfloat_offered_is_kept")
{
	// VK_FORMAT_R16G16B16A16_SFLOAT
	CheckChoice(ChooseSwapchainFormat(GfxApiKind::Vulkan, 97, vr::ColorSpace_Auto, { 97, 43 }), 97, false);
	// VK_FORMAT_R8G8B8A8_UNORM under Auto: VK_FORMAT_R8G8B8A8_SRGB
	CheckChoice(ChooseSwapchainFormat(GfxApiKind::Vulkan, 37, vr::ColorSpace_Auto, { 43, 37 }), 43, false);
	// VK_FORMAT_B10G11R11_UFLOAT_PACK32 with only RGBA8 sRGB
	CheckChoice(ChooseSwapchainFormat(GfxApiKind::Vulkan, 122, vr::ColorSpace_Auto, { 43 }), 43, true);
}

TEST_CASE("Unknown_format_is_tried_as_it_is")
{
	// DXGI_FORMAT_R8G8B8A8_SNORM and DXGI_FORMAT_R10G10B10A2_UINT, offered or not
	CheckChoice(ChooseSwapchainFormat(GfxApiKind::D3D11, 31, vr::ColorSpace_Auto, { 29 }), 31, false);
	CheckChoice(ChooseSwapchainFormat(GfxApiKind::D3D12, 25, vr::ColorSpace_Auto, { 25, 29 }), 25, false);
	CheckChoice(ChooseSwapchainFormat(GfxApiKind::D3D11, 9999, vr::ColorSpace_Auto, { 29 }), 9999, false);
	// VK_FORMAT_A8B8G8R8_UNORM_PACK32
	CheckChoice(ChooseSwapchainFormat(GfxApiKind::Vulkan, 51, vr::ColorSpace_Auto, { 43 }), 51, false);
	// The runtime couldn't say what it has
	CheckChoice(ChooseSwapchainFormat(GfxApiKind::D3D11, 31, vr::ColorSpace_Auto, {}), 31, false);
	// OpenGL: GL_RGB565 as it is where offered, else blitted into the runtime's first 8-bit RGBA or BGRA format
	CheckChoice(ChooseSwapchainFormat(GfxApiKind::OpenGL, 0x8D62, vr::ColorSpace_Auto, { 0x8D62, 0x8C43 }), 0x8D62, false);
	CheckChoice(ChooseSwapchainFormat(GfxApiKind::OpenGL, 0x8D62, vr::ColorSpace_Auto, { 0x881A, 0x8C43, 0x8058 }), 0x8C43, true);
	CheckChoice(ChooseSwapchainFormat(GfxApiKind::OpenGL, 0x8D62, vr::ColorSpace_Auto, { 0x881A }), 0x8D62, false);
}

TEST_CASE("Depth_and_compressed_beyond_bc1_are_nullopt")
{
	// Depth formats as colour: DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R24G8_TYPELESS, VK_FORMAT_D32_SFLOAT,
	// GL_DEPTH_COMPONENT32F, GL_DEPTH24_STENCIL8
	CHECK_FALSE(ChooseSwapchainFormat(GfxApiKind::D3D11, 40, vr::ColorSpace_Auto, { 29 }).has_value());
	CHECK_FALSE(ChooseSwapchainFormat(GfxApiKind::D3D12, 44, vr::ColorSpace_Auto, { 29 }).has_value());
	CHECK_FALSE(ChooseSwapchainFormat(GfxApiKind::Vulkan, 126, vr::ColorSpace_Auto, { 43 }).has_value());
	CHECK_FALSE(ChooseSwapchainFormat(GfxApiKind::OpenGL, 0x8CAC, vr::ColorSpace_Auto, { 0x8C43 }).has_value());
	CHECK_FALSE(ChooseSwapchainFormat(GfxApiKind::OpenGL, 0x88F0, vr::ColorSpace_Auto, { 0x8C43 }).has_value());
	// Block-compressed other than BC1: DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC7_UNORM_SRGB, VK_FORMAT_BC3_UNORM_BLOCK
	CHECK_FALSE(ChooseSwapchainFormat(GfxApiKind::D3D11, 77, vr::ColorSpace_Auto, { 29 }).has_value());
	CHECK_FALSE(ChooseSwapchainFormat(GfxApiKind::D3D12, 99, vr::ColorSpace_Auto, { 29 }).has_value());
	CHECK_FALSE(ChooseSwapchainFormat(GfxApiKind::Vulkan, 137, vr::ColorSpace_Auto, { 43 }).has_value());
	// Colour formats near them in the enums aren't: DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16_UNORM
	CheckChoice(ChooseSwapchainFormat(GfxApiKind::D3D11, 41, vr::ColorSpace_Auto, { 29 }), 41, false);
	CheckChoice(ChooseSwapchainFormat(GfxApiKind::D3D11, 56, vr::ColorSpace_Auto, { 29 }), 56, false);
	// BC1 is known: converted
	CheckChoice(ChooseSwapchainFormat(GfxApiKind::D3D11, 71, vr::ColorSpace_Auto, { 29 }), 29, true);
}

TEST_CASE("GL_rgba16f_to_srgb8_converts")
{
	// GL_RGBA16F with only GL_SRGB8_ALPHA8
	CheckChoice(ChooseSwapchainFormat(GfxApiKind::OpenGL, 0x881A, vr::ColorSpace_Auto, { 0x8C43 }), 0x8C43, true);
	// GL_RGBA8 under Gamma: GL_SRGB8_ALPHA8, a plain copy
	CheckChoice(ChooseSwapchainFormat(GfxApiKind::OpenGL, 0x8058, vr::ColorSpace_Gamma, { 0x8058, 0x8C43 }), 0x8C43, false);
	// Unsized GL_RGBA is GL_RGBA8
	CheckChoice(ChooseSwapchainFormat(GfxApiKind::OpenGL, 0x1908, vr::ColorSpace_Linear, { 0x8C43, 0x8058 }), 0x8058, false);
}

TEST_CASE("No_formats_known_keeps_todays_choice")
{
	// The runtime listed nothing: the family's member for the colour space, copied as today
	CheckChoice(ChooseSwapchainFormat(GfxApiKind::D3D11, 27, vr::ColorSpace_Gamma, {}), 29, false);
	CheckChoice(ChooseSwapchainFormat(GfxApiKind::Vulkan, 97, vr::ColorSpace_Auto, {}), 97, false);
}
