// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "Misc/Equirect.h"

namespace {
Image Flat(uint32_t rgba)
{
	Image img;
	img.width = img.height = 8;
	img.rgba.resize(8 * 8 * 4);
	for (size_t i = 0; i < img.rgba.size(); i += 4) {
		img.rgba[i] = (uint8_t)(rgba >> 24);
		img.rgba[i + 1] = (uint8_t)(rgba >> 16);
		img.rgba[i + 2] = (uint8_t)(rgba >> 8);
		img.rgba[i + 3] = (uint8_t)rgba;
	}
	return img;
}

struct Faces {
	Image front = Flat(0xff0000ff), back = Flat(0x00ff00ff), left = Flat(0x0000ffff), right = Flat(0xffff00ff),
	      top = Flat(0xff00ffff), bottom = Flat(0x00ffffff);
	std::array<const Image*, 6> All() const { return { &front, &back, &left, &right, &top, &bottom }; }
};
} // namespace

TEST_CASE("Equirect: the centre is the front face")
{
	Faces f;
	Image e = CubeFacesToEquirect(f.All(), 64);
	REQUIRE(e.width == 64);
	REQUIRE(e.height == 32);
	CHECK(e.At(32, 16) == 0xff0000ff);
	// Behind is the back face, at both ends of the middle row
	CHECK(e.At(0, 16) == 0x00ff00ff);
	CHECK(e.At(63, 16) == 0x00ff00ff);
}

TEST_CASE("Equirect: the top row is the top face, the bottom row the bottom face")
{
	Faces f;
	Image e = CubeFacesToEquirect(f.All(), 64);
	for (uint32_t x = 0; x < 64; x += 7) {
		CHECK(e.At(x, 0) == 0xff00ffff);
		CHECK(e.At(x, 31) == 0x00ffffff);
	}
}

TEST_CASE("Equirect: a quarter turn left is the left face, right the right face")
{
	Faces f;
	Image e = CubeFacesToEquirect(f.All(), 64);
	CHECK(e.At(16, 16) == 0x0000ffff);
	CHECK(e.At(48, 16) == 0xffff00ff);
}

TEST_CASE("Equirect: a missing face or a too small width gives nothing")
{
	Faces f;
	std::array<const Image*, 6> faces = f.All();
	faces[3] = nullptr;
	CHECK(CubeFacesToEquirect(faces, 64).Empty());
	CHECK(CubeFacesToEquirect(f.All(), 1).Empty());
}
