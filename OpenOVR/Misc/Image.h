#pragma once
// An RGBA image in memory: what SetOverlayRaw gives, what a texture reads back as, and what a swapchain filled from
// memory (CpuSwapchain) shows.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct Image {
	uint32_t width = 0, height = 0;
	std::vector<uint8_t> rgba; // RGBA8, rows top to bottom, sRGB-encoded

	// The pixel at (x, y) from the top-left, as 0xRRGGBBAA (0 outside the image)
	uint32_t At(uint32_t x, uint32_t y) const;
	bool Empty() const { return width == 0 || height == 0; }
};

// An image of 1, 3 or 4 bytes per pixel (as SetOverlayRaw allows: grey, RGB, RGBA), rows of stride bytes top to
// bottom, as RGBA8: a grey byte g becomes g g g 255, RGB gets alpha 255. An empty image for any other size, for no
// data, or for more than kMaxRawImageSide pixels on a side (the largest texture Direct3D 11 has).
constexpr uint32_t kMaxRawImageSide = 16384;
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

// A PNG file as an image (a JPEG is refused with a reason saying so), at most kMaxImageFileSide pixels on a side; nothing (with why) for a file that can't be read,
// isn't a PNG, is damaged or is too big (more than kMaxImageFileBytes, which is checked before it's read)
constexpr uint32_t kMaxImageFileSide = 8192;
constexpr uint64_t kMaxImageFileBytes = 64ull * 1024 * 1024;
std::optional<Image> LoadImageFile(const std::filesystem::path& path, std::string* why);

// An image as a PNG file (its folder made where missing); false, with why, if it can't be written
bool WritePngFile(const Image& image, const std::filesystem::path& path, std::string* why);

// Two images side by side, left's at the left, as tall as the taller (the space under the shorter is clear)
Image SideBySide(const Image& left, const Image& right);
