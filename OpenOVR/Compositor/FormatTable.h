#pragma once
// The game's texture formats against the runtime's swapchain formats: which swapchain format a texture goes into, and
// whether it can be copied there as it is or needs a pass that converts it (a shader or a blit).

#include "generated/interfaces/vrtypes.h"

#include <cstdint>
#include <optional>
#include <vector>

enum class GfxApiKind { D3D11, D3D12, Vulkan, OpenGL };

struct FormatChoice {
	int64_t swapchainFormat;
	bool convert; // a shader or blit pass is needed
};

// gameFormat is the API's own enum (DXGI_FORMAT, VkFormat, a GL internal format); colourSpace as OpenVR's EColorSpace;
// runtimeFormats in the runtime's order of preference. In order:
//  1. the game's format (or its typeless family member with the colour space's sRGB-ness) when the runtime offers it;
//  2. otherwise the runtime's first format of the same bit depth class;
//  3. otherwise its first 8-bit RGBA or BGRA format, with convert = true.
// With no runtime formats at all (it couldn't say), the family member as rule 1 picks it, copied as it is.
// A format no table knows is tried as it is (OpenGL: blitted into 8-bit RGBA or BGRA where the runtime lacks it);
// std::nullopt only for depth and stencil formats as colour, and DXGI's and Vulkan's block-compressed formats beyond BC1.
std::optional<FormatChoice> ChooseSwapchainFormat(GfxApiKind api, int64_t gameFormat, vr::EColorSpace colourSpace,
    const std::vector<int64_t>& runtimeFormats);

// A depth swapchain format of the runtime's for a game's depth texture format: the depth format of the same bits
// (DXGI's typeless and typed members alike), if it offers it
std::optional<int64_t> ChooseDepthSwapchainFormat(GfxApiKind api, int64_t gameFormat, const std::vector<int64_t>& runtimeFormats);

// For a copy between APIs (an OpenGL or Vulkan texture copied through D3D11): the format of another API that holds the
// same texels (the family's member of the same sRGB-ness; a typeless one as its linear member), or std::nullopt if that
// API's table has none, or the format is block-compressed or integer
std::optional<int64_t> SameTexelsIn(GfxApiKind from, int64_t format, GfxApiKind to);
// Where there's no such format, one a converting copy (an OpenGL blit) can write the values into without losing range:
// 8-bit RGBA of the same sRGB-ness for formats of 8 bits a channel or fewer, else 16-bit float RGBA; std::nullopt for
// formats no table knows, and block-compressed or integer ones
std::optional<int64_t> ConvertibleColourIn(GfxApiKind from, int64_t format, GfxApiKind to);

// Whether the format is an sRGB one, and its family member of the given sRGB-ness (the format itself if the family has
// none such); for views of a texture whose data a swapchain format reinterprets
bool FormatIsSrgb(GfxApiKind api, int64_t format);
int64_t FormatWithSrgb(GfxApiKind api, int64_t format, bool srgb);
// The colour space's sRGB-ness for the format: Gamma yes, Linear no, Auto at 8 bits a channel or fewer
bool WantsSrgb(GfxApiKind api, int64_t format, vr::EColorSpace colourSpace);
