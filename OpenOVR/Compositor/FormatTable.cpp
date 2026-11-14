#include "stdafx.h"

#include "FormatTable.h"

#include <algorithm>

namespace {

// How many bits a channel has: formats of one class hold the same range of values
enum class DepthClass { Low, Bits8, Bits10, Float16, Float32, Compressed };

// Formats whose texels are the same bits (a plain copy moves them), told apart by type: typeless, linear, sRGB
enum class Family {
	RGBA8,
	BGRA8,
	BGRX8,
	RGB8,
	RGBA16F,
	RGB16F,
	RGBA16,
	RGB10A2,
	BGR10A2,
	RGB10A2Uint,
	RG11B10F,
	RGB10XR,
	RGBA32F,
	RGB32F,
	B5G6R5,
	B5G5R5A1,
	B4G4R4A4,
	BC1,
	BC1RGB,
};

struct FormatInfo {
	int64_t format;
	Family family;
	DepthClass depth;
	enum Kind { Typeless, Linear, Srgb } kind;
	bool integer = false; // read as integers, not converted to colours: copied only
};

using K = FormatInfo::Kind;

// DXGI_FORMAT (D3D11, D3D12)
const std::vector<FormatInfo>& DxgiTable()
{
	static const std::vector<FormatInfo> table = {
		{ 27, Family::RGBA8, DepthClass::Bits8, K::Typeless },
		{ 28, Family::RGBA8, DepthClass::Bits8, K::Linear },
		{ 29, Family::RGBA8, DepthClass::Bits8, K::Srgb },
		{ 90, Family::BGRA8, DepthClass::Bits8, K::Typeless },
		{ 87, Family::BGRA8, DepthClass::Bits8, K::Linear },
		{ 91, Family::BGRA8, DepthClass::Bits8, K::Srgb },
		{ 92, Family::BGRX8, DepthClass::Bits8, K::Typeless },
		{ 88, Family::BGRX8, DepthClass::Bits8, K::Linear },
		{ 93, Family::BGRX8, DepthClass::Bits8, K::Srgb },
		// R16G16B16A16: typeless is read as float, the usual one
		{ 9, Family::RGBA16F, DepthClass::Float16, K::Typeless },
		{ 10, Family::RGBA16F, DepthClass::Float16, K::Linear },
		{ 11, Family::RGBA16, DepthClass::Float16, K::Linear },
		{ 23, Family::RGB10A2, DepthClass::Bits10, K::Typeless },
		{ 24, Family::RGB10A2, DepthClass::Bits10, K::Linear },
		{ 26, Family::RG11B10F, DepthClass::Bits10, K::Linear },
		{ 89, Family::RGB10XR, DepthClass::Bits10, K::Linear },
		{ 1, Family::RGBA32F, DepthClass::Float32, K::Typeless },
		{ 2, Family::RGBA32F, DepthClass::Float32, K::Linear },
		{ 85, Family::B5G6R5, DepthClass::Low, K::Linear },
		{ 86, Family::B5G5R5A1, DepthClass::Low, K::Linear },
		{ 115, Family::B4G4R4A4, DepthClass::Low, K::Linear },
		{ 70, Family::BC1, DepthClass::Compressed, K::Typeless },
		{ 71, Family::BC1, DepthClass::Compressed, K::Linear },
		{ 72, Family::BC1, DepthClass::Compressed, K::Srgb },
	};
	return table;
}

// VkFormat
const std::vector<FormatInfo>& VkTable()
{
	static const std::vector<FormatInfo> table = {
		{ 37, Family::RGBA8, DepthClass::Bits8, K::Linear },
		{ 43, Family::RGBA8, DepthClass::Bits8, K::Srgb },
		{ 44, Family::BGRA8, DepthClass::Bits8, K::Linear },
		{ 50, Family::BGRA8, DepthClass::Bits8, K::Srgb },
		{ 97, Family::RGBA16F, DepthClass::Float16, K::Linear },
		{ 91, Family::RGBA16, DepthClass::Float16, K::Linear },
		{ 64, Family::RGB10A2, DepthClass::Bits10, K::Linear }, // A2B10G10R10_UNORM_PACK32: R in the low bits
		{ 58, Family::BGR10A2, DepthClass::Bits10, K::Linear }, // A2R10G10B10_UNORM_PACK32
		{ 62, Family::RGB10A2Uint, DepthClass::Bits10, K::Linear, true }, // A2R10G10B10_UINT_PACK32
		{ 122, Family::RG11B10F, DepthClass::Bits10, K::Linear },
		{ 109, Family::RGBA32F, DepthClass::Float32, K::Linear },
		{ 106, Family::RGB32F, DepthClass::Float32, K::Linear },
		{ 131, Family::BC1RGB, DepthClass::Compressed, K::Linear },
		{ 132, Family::BC1RGB, DepthClass::Compressed, K::Srgb },
		{ 133, Family::BC1, DepthClass::Compressed, K::Linear },
		{ 134, Family::BC1, DepthClass::Compressed, K::Srgb },
	};
	return table;
}

// OpenGL sized internal formats (GL_RGBA, unsized, is taken as GL_RGBA8)
const std::vector<FormatInfo>& GlTable()
{
	static const std::vector<FormatInfo> table = {
		{ 0x8058, Family::RGBA8, DepthClass::Bits8, K::Linear }, // GL_RGBA8
		{ 0x8C43, Family::RGBA8, DepthClass::Bits8, K::Srgb }, // GL_SRGB8_ALPHA8
		{ 0x8051, Family::RGB8, DepthClass::Bits8, K::Linear }, // GL_RGB8
		{ 0x8C41, Family::RGB8, DepthClass::Bits8, K::Srgb }, // GL_SRGB8
		{ 0x881A, Family::RGBA16F, DepthClass::Float16, K::Linear }, // GL_RGBA16F
		{ 0x881B, Family::RGB16F, DepthClass::Float16, K::Linear }, // GL_RGB16F
		{ 0x805B, Family::RGBA16, DepthClass::Float16, K::Linear }, // GL_RGBA16
		{ 0x8059, Family::RGB10A2, DepthClass::Bits10, K::Linear }, // GL_RGB10_A2
		{ 0x8C3A, Family::RG11B10F, DepthClass::Bits10, K::Linear }, // GL_R11F_G11F_B10F
		{ 0x8814, Family::RGBA32F, DepthClass::Float32, K::Linear }, // GL_RGBA32F
		{ 0x8815, Family::RGB32F, DepthClass::Float32, K::Linear }, // GL_RGB32F
		{ 0x83F1, Family::BC1, DepthClass::Compressed, K::Linear }, // GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
		{ 0x8C4D, Family::BC1, DepthClass::Compressed, K::Srgb }, // GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT
	};
	return table;
}

const std::vector<FormatInfo>& TableOf(GfxApiKind api)
{
	switch (api) {
	case GfxApiKind::D3D11:
	case GfxApiKind::D3D12:
		return DxgiTable();
	case GfxApiKind::Vulkan:
		return VkTable();
	default:
		return GlTable();
	}
}

int64_t Normalised(GfxApiKind api, int64_t format)
{
	if (api == GfxApiKind::OpenGL && format == 0x1908) // GL_RGBA
		return 0x8058;
	return format;
}

const FormatInfo* Find(GfxApiKind api, int64_t format)
{
	format = Normalised(api, format);
	for (const FormatInfo& f : TableOf(api))
		if (f.format == format)
			return &f;
	return nullptr;
}

// The family's member of a kind (Linear or Srgb), or nullptr
const FormatInfo* Member(GfxApiKind api, Family family, K kind)
{
	for (const FormatInfo& f : TableOf(api))
		if (f.family == family && f.kind == kind)
			return &f;
	return nullptr;
}

bool Offered(const std::vector<int64_t>& formats, int64_t f)
{
	return std::find(formats.begin(), formats.end(), f) != formats.end();
}

} // namespace

bool WantsSrgb(GfxApiKind api, int64_t format, vr::EColorSpace colourSpace)
{
	switch (colourSpace) {
	case vr::ColorSpace_Gamma:
		return true;
	case vr::ColorSpace_Linear:
		return false;
	default: {
		// As the docs for the auto mode say: at eight bits a channel or fewer it's gamma (linear colour in so few bits
		// would band badly)
		const FormatInfo* info = Find(api, format);
		return info && (info->depth == DepthClass::Bits8 || info->depth == DepthClass::Low || info->depth == DepthClass::Compressed);
	}
	}
}

std::optional<FormatChoice> ChooseSwapchainFormat(GfxApiKind api, int64_t gameFormat, vr::EColorSpace colourSpace,
    const std::vector<int64_t>& runtimeFormats)
{
	const FormatInfo* game = Find(api, gameFormat);
	if (!game)
		return std::nullopt;

	// The member of the game's family the colour space asks for (a typeless format has none of its own)
	const bool srgb = WantsSrgb(api, gameFormat, colourSpace);
	const FormatInfo* wanted = Member(api, game->family, srgb ? K::Srgb : K::Linear);
	if (!wanted)
		wanted = Member(api, game->family, K::Linear);
	if (!wanted)
		wanted = game;

	// The runtime couldn't say what it has: that member, as before
	if (runtimeFormats.empty())
		return FormatChoice{ wanted->format, false };

	// 1. The game's family: the member wanted, the game's own, then any other (all plain copies)
	if (Offered(runtimeFormats, wanted->format))
		return FormatChoice{ wanted->format, false };
	if (game->kind != K::Typeless && Offered(runtimeFormats, game->format))
		return FormatChoice{ game->format, false };
	for (int64_t f : runtimeFormats) {
		const FormatInfo* info = Find(api, f);
		if (info && info->family == game->family && info->kind != K::Typeless)
			return FormatChoice{ f, false };
	}

	// Integers aren't colours a pass could convert: tried as they are
	if (game->integer)
		return FormatChoice{ wanted->format, false };

	// 2. The runtime's first format of the same bit depth class, converted
	if (game->depth != DepthClass::Low && game->depth != DepthClass::Compressed) {
		for (int64_t f : runtimeFormats) {
			const FormatInfo* info = Find(api, f);
			if (info && info->depth == game->depth && info->kind != K::Typeless && !info->integer)
				return FormatChoice{ f, true };
		}
	}

	// 3. Its first 8-bit RGBA or BGRA format, converted
	for (int64_t f : runtimeFormats) {
		const FormatInfo* info = Find(api, f);
		if (info && info->kind != K::Typeless && (info->family == Family::RGBA8 || info->family == Family::BGRA8))
			return FormatChoice{ f, true };
	}

	// Nothing to convert into: tried as it is, as before
	return FormatChoice{ wanted->format, false };
}

std::optional<int64_t> ChooseDepthSwapchainFormat(GfxApiKind api, int64_t gameFormat, const std::vector<int64_t>& runtimeFormats)
{
	// Each family: the depth format a swapchain is made in, and the game's formats of the same bits
	struct DepthFamily {
		int64_t swapchain;
		std::vector<int64_t> members;
	};
	static const std::vector<DepthFamily> dxgi = {
		{ 40, { 39, 40, 41 } }, // D32_FLOAT: R32_TYPELESS, D32_FLOAT, R32_FLOAT
		{ 45, { 44, 45, 46 } }, // D24_UNORM_S8_UINT: R24G8_TYPELESS, D24_UNORM_S8_UINT, R24_UNORM_X8_TYPELESS
		{ 55, { 53, 55, 56 } }, // D16_UNORM: R16_TYPELESS, D16_UNORM, R16_UNORM
		{ 20, { 19, 20, 21 } }, // D32_FLOAT_S8X24_UINT: R32G8X24_TYPELESS, D32_FLOAT_S8X24_UINT, R32_FLOAT_X8X24_TYPELESS
	};
	static const std::vector<DepthFamily> vk = {
		{ 126, { 126 } }, // D32_SFLOAT
		{ 129, { 129 } }, // D24_UNORM_S8_UINT
		{ 124, { 124 } }, // D16_UNORM
		{ 130, { 130 } }, // D32_SFLOAT_S8_UINT
	};
	static const std::vector<DepthFamily> gl = {
		{ 0x8CAC, { 0x8CAC } }, // GL_DEPTH_COMPONENT32F
		{ 0x88F0, { 0x88F0 } }, // GL_DEPTH24_STENCIL8
		{ 0x81A6, { 0x81A6 } }, // GL_DEPTH_COMPONENT24
		{ 0x81A5, { 0x81A5 } }, // GL_DEPTH_COMPONENT16
		{ 0x8CAD, { 0x8CAD } }, // GL_DEPTH32F_STENCIL8
	};
	const std::vector<DepthFamily>& families = api == GfxApiKind::Vulkan ? vk : api == GfxApiKind::OpenGL ? gl : dxgi;
	for (const DepthFamily& family : families) {
		if (std::find(family.members.begin(), family.members.end(), gameFormat) == family.members.end())
			continue;
		if (Offered(runtimeFormats, family.swapchain))
			return family.swapchain;
		return std::nullopt;
	}
	return std::nullopt;
}

bool FormatKnown(GfxApiKind api, int64_t format)
{
	return Find(api, format) != nullptr;
}

bool FormatIsSrgb(GfxApiKind api, int64_t format)
{
	const FormatInfo* info = Find(api, format);
	return info && info->kind == K::Srgb;
}

int64_t FormatWithSrgb(GfxApiKind api, int64_t format, bool srgb)
{
	const FormatInfo* info = Find(api, format);
	if (!info)
		return format;
	const FormatInfo* member = Member(api, info->family, srgb ? K::Srgb : K::Linear);
	return member ? member->format : Normalised(api, format);
}
