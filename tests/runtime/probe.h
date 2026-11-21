#pragma once
// Pixel readback of the stand-in runtime's swapchain images, for the dump's layer records: the values at a rect's
// four corners (2 pixels in) and at its centre, as RGBA8.

#include "runtime_internal.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// RGBA8 values, 0xRRGGBBAA, of the rect as shown: tl is its top-left corner whatever the API's row order. They are
// the bytes the image stores: an sRGB format's sRGB-encoded bytes, a UNORM one's linear bytes, with no conversion
// between them; float and 10-bit channels are scaled to 8 bits. The record's format says how to read them.
struct Samples {
	uint32_t tl = 0, tr = 0, bl = 0, br = 0, c = 0;
};

struct SwapchainImage {
	RtSession* session = nullptr;
	RtSwapchain* swapchain = nullptr;
	uint32_t index = 0;
};

#ifdef _WIN32
// The layout of a DXGI format's texels (D3D11 and D3D12)
PixelLayout LayoutOfDxgi(int64_t format);
#endif

// Reads the samples of rect in layer arrayIndex of the image, converted to RGBA8, through the session's API (each
// API's staging path, RtGfx::read; a 32-bit float depth image's value in every channel). False when it can't be read
// (no such image, another depth format or an unknown one, an empty rect, a multisampled image on D3D12, Vulkan or
// OpenGL).
// extra: more points to read in the same readback, as (x, y) fractions of the rect from its top-left as shown, their
// texels in extraOut.
bool ProbeSamples(SwapchainImage& image, XrRect2Di rect, uint32_t arrayIndex, Samples& out, std::string* why = nullptr,
    const std::vector<std::pair<float, float>>* extra = nullptr, std::vector<uint32_t>* extraOut = nullptr);

// {"tl": "ff0000ff", "tr": ..., "bl": ..., "br": ..., "c": ...}
Json::Value SamplesJson(const Samples& s);
