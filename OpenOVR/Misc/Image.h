#pragma once
// An RGBA image in memory: what SetOverlayRaw gives, what a texture reads back as, and what a swapchain filled from
// memory (CpuSwapchain) shows.

#include <cstdint>
#include <vector>

struct Image {
	uint32_t width = 0, height = 0;
	std::vector<uint8_t> rgba; // RGBA8, rows top to bottom, sRGB-encoded

	// The pixel at (x, y) from the top-left, as 0xRRGGBBAA (0 outside the image)
	uint32_t At(uint32_t x, uint32_t y) const;
	bool Empty() const { return width == 0 || height == 0; }
};

// An image of 1, 3 or 4 bytes per pixel (as SetOverlayRaw allows: grey, RGB, RGBA), rows of stride bytes top to
// bottom, as RGBA8: a grey byte g becomes g g g 255, RGB gets alpha 255. An empty image for any other size, or for
// no data.
Image ConvertToRgba8(const void* data, uint32_t width, uint32_t height, uint32_t stride, uint32_t bytesPerPixel);

// How a texture's texels are laid out in memory, for reading them back
enum class TexelLayout {
	Unknown,
	RGBA8,
	BGRA8,
	BGRX8, // alpha read as 255
	RGBA16F,
	RGB10A2,
	RG11B10F,
	R32F, // depth: the value in every channel
};

// The bytes of one texel of the layout (0 for Unknown)
uint32_t BytesOfTexelLayout(TexelLayout layout);

// Whether texels of the layout hold linear values that need sRGB encoding when read back: float and 10-bit ones,
// unless the texture's colour space says they're gamma-encoded
inline bool TexelsAreLinear(TexelLayout layout, bool gammaColourSpace)
{
	bool wide = layout == TexelLayout::RGBA16F || layout == TexelLayout::RGB10A2 || layout == TexelLayout::RG11B10F;
	return wide && !gammaColourSpace;
}

// sRGB-encodes the colour channels of an image whose values are linear
void EncodeSrgbInPlace(Image& image);

// Texels of the layout (rows of stride bytes; bottomUp: the first row is the bottom one, as OpenGL's) as an Image.
// encodeSrgb: the values are linear (float and 10-bit formats under a linear colour space) and are sRGB-encoded;
// 8-bit values are taken as they are.
Image DecodeTexels(TexelLayout layout, const void* data, uint32_t width, uint32_t height, size_t stride, bool bottomUp,
    bool encodeSrgb);
