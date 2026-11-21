// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

#include "Misc/Image.h"

#include <cstring>
#include <vector>

TEST_CASE("Image_convert_1_3_4_bytes")
{
	// Grey: g g g 255
	const uint8_t grey[4] = { 0x10, 0x20, 0x30, 0x40 };
	Image g = ConvertToRgba8(grey, 2, 2, 2, 1);
	REQUIRE(g.width == 2);
	REQUIRE(g.height == 2);
	REQUIRE(g.rgba.size() == 16);
	CHECK(g.At(0, 0) == 0x101010ffu);
	CHECK(g.At(1, 0) == 0x202020ffu);
	CHECK(g.At(0, 1) == 0x303030ffu);
	CHECK(g.At(1, 1) == 0x404040ffu);

	// RGB keeps its order, with alpha 255; the stride may be longer than a row
	const uint8_t rgb[2 * 8] = { 1, 2, 3, 4, 5, 6, 0xEE, 0xEE, 7, 8, 9, 10, 11, 12, 0xEE, 0xEE };
	Image c = ConvertToRgba8(rgb, 2, 2, 8, 3);
	REQUIRE(c.rgba.size() == 16);
	CHECK(c.At(0, 0) == 0x010203ffu);
	CHECK(c.At(1, 0) == 0x040506ffu);
	CHECK(c.At(0, 1) == 0x070809ffu);
	CHECK(c.At(1, 1) == 0x0a0b0cffu);

	// RGBA is copied as it is
	const uint8_t rgba[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	Image a = ConvertToRgba8(rgba, 2, 1, 8, 4);
	REQUIRE(a.rgba.size() == 8);
	CHECK(std::memcmp(a.rgba.data(), rgba, 8) == 0);
	CHECK(a.At(1, 0) == 0x05060708u);
	CHECK(a.At(2, 0) == 0u); // outside

	// Sizes SetOverlayRaw doesn't take, a stride shorter than a row, or no data: empty
	CHECK(ConvertToRgba8(rgba, 2, 1, 8, 2).Empty());
	CHECK(ConvertToRgba8(rgba, 2, 1, 4, 4).Empty());
	CHECK(ConvertToRgba8(nullptr, 2, 1, 8, 4).Empty());
	CHECK(ConvertToRgba8(rgba, 0, 1, 8, 4).Empty());
	// More pixels on a side than any texture has: refused before anything is read
	CHECK(ConvertToRgba8(rgba, kMaxRawImageSide + 1, 1, (kMaxRawImageSide + 1) * 4, 4).Empty());
	CHECK(ConvertToRgba8(rgba, 1, kMaxRawImageSide + 1, 4, 4).Empty());
}

TEST_CASE("Image_decode_texels_refuses_a_side_no_texture_has")
{
	const uint32_t tall = kMaxRawImageSide + 1;
	std::vector<uint8_t> texels((size_t)tall * 4, 0x80);
	Image image = DecodeTexels(TexelLayout::RGBA8, texels.data(), 1, tall, 4, false, false);
	CHECK(image.width == 0);
	CHECK(image.rgba.empty());
	CHECK(DecodeTexels(TexelLayout::RGBA8, texels.data(), 1, kMaxRawImageSide, 4, false, false).height == kMaxRawImageSide);
}

TEST_CASE("Image_decode_texels")
{
	// BGRA8 swapped, bottom-up rows put top-down
	const uint8_t bgra[8] = { 3, 2, 1, 4, 7, 6, 5, 8 }; // row 0 (bottom), row 1 (top)
	Image b = DecodeTexels(TexelLayout::BGRA8, bgra, 1, 2, 4, true, false);
	REQUIRE(b.height == 2);
	CHECK(b.At(0, 0) == 0x05060708u);
	CHECK(b.At(0, 1) == 0x01020304u);

	// Half floats 0 and 1, sRGB-encoded (0 and 1 stay 0 and 255)
	const uint16_t half[4] = { 0x3C00, 0, 0x3C00, 0x3C00 };
	CHECK(DecodeTexels(TexelLayout::RGBA16F, half, 1, 1, 8, false, true).At(0, 0) == 0xff00ffffu);
	// A linear 0.5 encodes as sRGB 188
	const uint16_t halfMid[4] = { 0x3800, 0x3800, 0x3800, 0x3C00 };
	CHECK(DecodeTexels(TexelLayout::RGBA16F, halfMid, 1, 1, 8, false, true).At(0, 0) == 0xbcbcbcffu);

	// R11G11B10 float: 1.0 is 0x3C0 (11 bits) and 0x1E0 (10 bits)
	const uint32_t packed = 0x3C0u | (0u << 11) | (0x1E0u << 22);
	CHECK(DecodeTexels(TexelLayout::RG11B10F, &packed, 1, 1, 4, false, false).At(0, 0) == 0xff00ffffu);

	// R10G10B10A2: all ones
	const uint32_t ten = 0xffffffffu;
	CHECK(DecodeTexels(TexelLayout::RGB10A2, &ten, 1, 1, 4, false, false).At(0, 0) == 0xffffffffu);

	// BGRX: alpha 255
	const uint8_t bgrx[4] = { 3, 2, 1, 0 };
	CHECK(DecodeTexels(TexelLayout::BGRX8, bgrx, 1, 1, 4, false, false).At(0, 0) == 0x010203ffu);

	// Depth 0.25: 0x40 in every channel
	const float depth = 0.25f;
	CHECK(DecodeTexels(TexelLayout::R32F, &depth, 1, 1, 4, false, false).At(0, 0) == 0x40404040u);

	CHECK(DecodeTexels(TexelLayout::Unknown, bgra, 1, 1, 4, false, false).Empty());
}
