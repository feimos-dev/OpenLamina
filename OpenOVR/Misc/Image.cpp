#include "stdafx.h"

#include "Image.h"

#include "lodepng.h"

#include <fstream>
#include <iterator>

#include <cmath>
#include <cstring>
#include <new>

uint32_t Image::At(uint32_t x, uint32_t y) const
{
	if (x >= width || y >= height || rgba.size() < (size_t)width * height * 4)
		return 0;
	const uint8_t* p = &rgba[((size_t)y * width + x) * 4];
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

Image ConvertToRgba8(const void* data, uint32_t width, uint32_t height, uint32_t stride, uint32_t bytesPerPixel)
{
	Image out;
	if (!data || !width || !height || (bytesPerPixel != 1 && bytesPerPixel != 3 && bytesPerPixel != 4))
		return out;
	// A size no texture has: its bytes wouldn't fit a 32-bit size
	if (width > kMaxRawImageSide || height > kMaxRawImageSide)
		return out;
	if ((uint64_t)stride < (uint64_t)width * bytesPerPixel)
		return out;
	try {
		out.rgba.resize((size_t)width * height * 4);
	} catch (const std::bad_alloc&) {
		return out;
	}
	out.width = width;
	out.height = height;
	const auto* src = (const uint8_t*)data;
	for (uint32_t y = 0; y < height; y++) {
		const uint8_t* row = src + (size_t)y * stride;
		uint8_t* dst = &out.rgba[(size_t)y * width * 4];
		if (bytesPerPixel == 4) {
			std::memcpy(dst, row, (size_t)width * 4);
			continue;
		}
		for (uint32_t x = 0; x < width; x++, dst += 4) {
			if (bytesPerPixel == 1) {
				dst[0] = dst[1] = dst[2] = row[x];
			} else {
				dst[0] = row[x * 3], dst[1] = row[x * 3 + 1], dst[2] = row[x * 3 + 2];
			}
			dst[3] = 255;
		}
	}
	return out;
}

uint32_t BytesOfTexelLayout(TexelLayout layout)
{
	switch (layout) {
	case TexelLayout::RGBA8:
	case TexelLayout::BGRA8:
	case TexelLayout::BGRX8:
	case TexelLayout::RGB10A2:
	case TexelLayout::RG11B10F:
	case TexelLayout::R32F:
		return 4;
	case TexelLayout::RGBA16F:
		return 8;
	default:
		return 0;
	}
}

namespace {

float HalfToFloat(uint16_t h)
{
	uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 31, mant = h & 1023;
	float v;
	if (exp == 0)
		v = std::ldexp((float)mant, -24);
	else if (exp == 31)
		v = mant ? NAN : INFINITY;
	else
		v = std::ldexp((float)(mant | 1024), (int)exp - 25);
	return sign ? -v : v;
}

// An unsigned float of the packed R11G11B10 kind: 5 exponent bits, mantissaBits mantissa bits, no sign
float SmallFloat(uint32_t bits, int mantissaBits)
{
	uint32_t exp = bits >> mantissaBits, mant = bits & ((1u << mantissaBits) - 1);
	if (exp == 0)
		return std::ldexp((float)mant, -14 - mantissaBits);
	if (exp == 31)
		return mant ? NAN : INFINITY;
	return std::ldexp((float)(mant | (1u << mantissaBits)), (int)exp - 15 - mantissaBits);
}

uint8_t Unorm8(float v)
{
	if (!(v > 0))
		return 0;
	if (v >= 1)
		return 255;
	return (uint8_t)std::lround(v * 255.0f);
}

uint8_t Encode(float v, bool srgb)
{
	if (srgb && v > 0 && v < 1)
		v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
	return Unorm8(v);
}

} // namespace

Image DecodeTexels(TexelLayout layout, const void* data, uint32_t width, uint32_t height, size_t stride, bool bottomUp, bool encodeSrgb)
{
	Image out;
	uint32_t bytes = BytesOfTexelLayout(layout);
	if (!data || !bytes || !width || !height || stride < (size_t)width * bytes)
		return out;
	out.width = width;
	out.height = height;
	out.rgba.resize((size_t)width * height * 4);
	for (uint32_t y = 0; y < height; y++) {
		const uint8_t* row = (const uint8_t*)data + (size_t)(bottomUp ? height - 1 - y : y) * stride;
		uint8_t* dst = &out.rgba[(size_t)y * width * 4];
		for (uint32_t x = 0; x < width; x++, dst += 4) {
			const uint8_t* t = row + (size_t)x * bytes;
			switch (layout) {
			case TexelLayout::RGBA8:
				dst[0] = t[0], dst[1] = t[1], dst[2] = t[2], dst[3] = t[3];
				break;
			case TexelLayout::BGRA8:
			case TexelLayout::BGRX8:
				dst[0] = t[2], dst[1] = t[1], dst[2] = t[0], dst[3] = layout == TexelLayout::BGRX8 ? 255 : t[3];
				break;
			case TexelLayout::RGBA16F: {
				uint16_t h[4];
				std::memcpy(h, t, 8);
				for (int c = 0; c < 3; c++)
					dst[c] = Encode(HalfToFloat(h[c]), encodeSrgb);
				dst[3] = Unorm8(HalfToFloat(h[3]));
				break;
			}
			case TexelLayout::RGB10A2: {
				uint32_t v;
				std::memcpy(&v, t, 4);
				for (int c = 0; c < 3; c++)
					dst[c] = Encode(((v >> (10 * c)) & 1023) / 1023.0f, encodeSrgb);
				dst[3] = (uint8_t)(((v >> 30) & 3) * 85);
				break;
			}
			case TexelLayout::RG11B10F: {
				uint32_t v;
				std::memcpy(&v, t, 4);
				dst[0] = Encode(SmallFloat(v & 0x7FF, 6), encodeSrgb);
				dst[1] = Encode(SmallFloat((v >> 11) & 0x7FF, 6), encodeSrgb);
				dst[2] = Encode(SmallFloat((v >> 22) & 0x3FF, 5), encodeSrgb);
				dst[3] = 255;
				break;
			}
			case TexelLayout::R32F: {
				float f;
				std::memcpy(&f, t, 4);
				dst[0] = dst[1] = dst[2] = dst[3] = Unorm8(f);
				break;
			}
			default:
				break;
			}
		}
	}
	return out;
}

void EncodeSrgbInPlace(Image& image)
{
	uint8_t table[256];
	for (int i = 0; i < 256; i++)
		table[i] = Encode(i / 255.0f, true);
	for (size_t i = 0; i + 3 < image.rgba.size(); i += 4)
		for (int c = 0; c < 3; c++)
			image.rgba[i + c] = table[image.rgba[i + c]];
}

std::optional<Image> LoadImageFile(const std::filesystem::path& path, std::string* why)
{
	auto fail = [&](const char* reason) -> std::optional<Image> {
		if (why)
			*why = reason;
		return std::nullopt;
	};
	std::ifstream in(path, std::ios::binary);
	if (!in)
		return fail("it can't be opened");
	std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	unsigned width = 0, height = 0;
	lodepng::State state;
	if (lodepng_inspect(&width, &height, &state, bytes.data(), bytes.size()) != 0)
		return fail("it isn't a PNG image");
	if (width == 0 || height == 0 || width > kMaxImageFileSide || height > kMaxImageFileSide)
		return fail("it's bigger than 8192 pixels on a side");
	Image image;
	if (lodepng::decode(image.rgba, width, height, bytes, LCT_RGBA, 8) != 0)
		return fail("it's damaged");
	image.width = width;
	image.height = height;
	return image;
}
