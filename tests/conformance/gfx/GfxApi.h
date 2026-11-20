#pragma once
// The game's side of the frame cases (gfx_cases.cpp): a device of one graphics API, and textures of a test pattern to
// submit through it. The device is the hardware adapter's (the first Vulkan physical device), or WARP for the D3D
// kinds when there is none.
//
// The pattern: quadrants top-left red, top-right green, bottom-left blue, bottom-right white, as shown in the
// headset (an OpenGL texture's rows run bottom-up). Eye 1 has them rotated once clockwise: top-left blue, top-right
// red, bottom-right green, bottom-left white.
//
// In the middle of both, a quarter of each side wide, a mid-grey block (0x80 as an sRGB byte): the corners' 0 and 255
// are the same in sRGB and linear, the grey isn't, so a conversion that decodes or encodes once too often shows in
// it. Float and 10-bit formats hold linear values, and get the grey's linear value (0.216). Textures under 8 pixels
// on a side, and multisampled ones, have the quadrants alone.

#include "callers_support.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

enum class GfxKind { D3D11, D3D12, Vulkan, OpenGL, D3D10 };

// A texture to submit: tex.handle points into vk or d3d12 for those APIs (Texture() sets it, so a copied GfxTexture
// still works); keep is the API's own object (ID3D11Texture2D*, ID3D12Resource*, VkImage, GL texture name,
// ID3D10Texture2D*), owned by the GfxApi that made it. A texture that couldn't be made has a null tex.handle.
struct GfxTexture {
	vr::Texture_t tex{};
	vr::VRVulkanTextureData_t vk{};
	vr::D3D12TextureData_t d3d12{};
	void* keep = nullptr;

	vr::Texture_t* Texture()
	{
		if (tex.eType == vr::TextureType_Vulkan)
			tex.handle = keep ? &vk : nullptr;
		else if (tex.eType == vr::TextureType_DirectX12)
			tex.handle = keep ? &d3d12 : nullptr;
		return &tex;
	}
	// Vulkan's VkImage is 64-bit on 32-bit Windows too, more than keep holds there
	bool Made() const { return tex.eType == vr::TextureType_Vulkan ? vk.m_nImage != 0 : keep != nullptr; }
};

class GfxApi {
public:
	// nullptr, with why in *whyNot, when no device of the kind can be made here
	static std::unique_ptr<GfxApi> Create(GfxKind kind, std::string* whyNot); // WARP for the D3D kinds when no adapter
	// The test pattern (above), w x h, in nativeFormat (the API's own enum value), with samples samples and arraySize
	// layers: layer i has the pattern of eye (eye + i) % 2, so a layer shown is told from another (multisampled
	// arrays: each layer the same)
	virtual GfxTexture Pattern(uint32_t w, uint32_t h, int64_t nativeFormat, int eye, uint32_t samples = 1, uint32_t arraySize = 1) = 0;
	// A depth texture (32-bit float) filled with value
	virtual GfxTexture Depth(uint32_t w, uint32_t h, float value) = 0;
	// Waits until everything the device was asked to do is done
	virtual void Flush() = 0;
	virtual ~GfxApi() = default;
};

namespace gfx {

// The block in the pattern's middle, 0xRRGGBBAA
constexpr uint32_t kPatternGrey = 0x808080ff;

// The pattern's colour at (x, y) from the top-left as shown, 0xRRGGBBAA
inline uint32_t PatternColour(uint32_t x, uint32_t y, uint32_t w, uint32_t h, int eye)
{
	static const uint32_t quadrants[4] = { 0xff0000ff, 0x00ff00ff, 0x0000ffff, 0xffffffff }; // tl, tr, bl, br
	if (w >= 8 && h >= 8 && x >= w * 3 / 8 && x < w * 5 / 8 && y >= h * 3 / 8 && y < h * 5 / 8)
		return kPatternGrey;
	int q = (y >= h / 2 ? 2 : 0) + (x >= w / 2 ? 1 : 0);
	if (eye == 1) {
		// Once clockwise: the top-left shows what was bottom-left, and so on round
		static const int from[4] = { 2, 0, 3, 1 };
		q = from[q];
	}
	return quadrants[q];
}

// How a format's texels are laid out in memory
enum class Texels { Unknown, RGBA8, BGRA8, BGRX8, RGBA16F, RGB10A2, RG11B10F };

// An sRGB byte's linear value, 0 to 1
inline float SrgbToLinear(uint8_t v)
{
	float c = v / 255.0f;
	return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// A value from 0 to 1 as an unsigned float of a 5-bit exponent and mantissaBits of mantissa: a half float's low 15
// bits (10), and the 11- and 10-bit floats of R11G11B10 (6 and 5). Values too small for a normal number become 0.
inline uint32_t SmallFloat(float f, int mantissaBits)
{
	if (!(f > 0))
		return 0;
	if (f > 1)
		f = 1;
	int e = 0;
	float m = std::frexp(f, &e); // f = m * 2^e, m from 0.5 to 1
	int exponent = e - 1 + 15;
	if (exponent <= 0)
		return 0;
	const uint32_t one = 1u << mantissaBits;
	uint32_t mantissa = (uint32_t)std::lround((2 * m - 1) * one);
	if (mantissa == one) {
		mantissa = 0;
		exponent++;
	}
	return ((uint32_t)exponent << mantissaBits) | mantissa;
}

inline uint16_t FloatToHalf(float f)
{
	return (uint16_t)SmallFloat(f, 10);
}

inline uint32_t BytesOf(Texels t)
{
	return t == Texels::RGBA16F ? 8 : t == Texels::Unknown ? 0 : 4;
}

// A colour (sRGB bytes) as a texel of the layout. The float and 10-bit layouts get its linear value; so do RGBA8
// bytes with linearBytes, for OpenGL to convert into such a format.
inline void WriteTexel(Texels t, uint32_t rgba, uint8_t* out, bool linearBytes = false)
{
	uint8_t r = (uint8_t)(rgba >> 24), g = (uint8_t)(rgba >> 16), b = (uint8_t)(rgba >> 8), a = (uint8_t)rgba;
	const float lr = SrgbToLinear(r), lg = SrgbToLinear(g), lb = SrgbToLinear(b);
	switch (t) {
	case Texels::RGBA8:
		if (linearBytes)
			r = (uint8_t)std::lround(lr * 255), g = (uint8_t)std::lround(lg * 255), b = (uint8_t)std::lround(lb * 255);
		out[0] = r, out[1] = g, out[2] = b, out[3] = a;
		break;
	case Texels::BGRA8:
		out[0] = b, out[1] = g, out[2] = r, out[3] = a;
		break;
	case Texels::BGRX8:
		out[0] = b, out[1] = g, out[2] = r, out[3] = 0; // the X byte: unused
		break;
	case Texels::RGBA16F: {
		uint16_t h[4] = { FloatToHalf(lr), FloatToHalf(lg), FloatToHalf(lb), FloatToHalf(a / 255.0f) };
		std::memcpy(out, h, 8);
		break;
	}
	case Texels::RGB10A2: {
		uint32_t v = (uint32_t)std::lround(lr * 1023) | ((uint32_t)std::lround(lg * 1023) << 10) | ((uint32_t)std::lround(lb * 1023) << 20)
		    | ((uint32_t)(a * 3 / 255) << 30);
		std::memcpy(out, &v, 4);
		break;
	}
	case Texels::RG11B10F: {
		// Unsigned small floats: 1.0 is exponent 15, mantissa 0 (0x3C0 in 11 bits, 0x1E0 in 10)
		uint32_t v = SmallFloat(lr, 6) | (SmallFloat(lg, 6) << 11) | (SmallFloat(lb, 5) << 22);
		std::memcpy(out, &v, 4);
		break;
	}
	default:
		break;
	}
}

// The pattern's texels, rowPitch bytes a row; bottomUp puts the bottom row (as shown) first; linearBytes as WriteTexel's
inline std::vector<uint8_t> PatternTexels(Texels t, uint32_t w, uint32_t h, int eye, size_t rowPitch, bool bottomUp, bool linearBytes = false)
{
	std::vector<uint8_t> data(rowPitch * h);
	uint32_t bytes = BytesOf(t);
	for (uint32_t row = 0; row < h; row++) {
		uint32_t y = bottomUp ? h - 1 - row : row;
		for (uint32_t x = 0; x < w; x++)
			WriteTexel(t, PatternColour(x, y, w, h, eye), &data[row * rowPitch + (size_t)x * bytes], linearBytes);
	}
	return data;
}

} // namespace gfx

// gfx/*Gfx.cpp: Vulkan and OpenGL everywhere, the D3D kinds on Windows
std::unique_ptr<GfxApi> MakeVkGfx(std::string* whyNot);
std::unique_ptr<GfxApi> MakeGLGfx(std::string* whyNot);
#ifdef _WIN32
std::unique_ptr<GfxApi> MakeD3D11Gfx(std::string* whyNot);
std::unique_ptr<GfxApi> MakeD3D12Gfx(std::string* whyNot);
std::unique_ptr<GfxApi> MakeD3D10Gfx(std::string* whyNot);
gfx::Texels TexelsOfDxgi(int64_t format);
#endif
