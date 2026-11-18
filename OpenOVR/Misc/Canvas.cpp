#include "stdafx.h"

#include "Canvas.h"

#include "BuiltInResource.h"
#include "Keyboard/SudoFontMeta.h"
#include "resources.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>

namespace {

// The keyboard's font, built in; null if it can't be read
const SudoFontMeta* Font()
{
	static std::once_flag once;
	static std::unique_ptr<SudoFontMeta> font;
	std::call_once(once, [] {
		std::vector<char> meta = ReadBuiltInResource(RES_O_FNT_UBUNTU, RES_T_FNTMETA);
#ifdef _WIN32
		std::vector<char> png = ReadBuiltInResource(RES_O_FNT_UBUNTU, RES_T_PNG);
#else
		// Each resource is one file here
		std::vector<char> png = ReadBuiltInResource(RES_O_FNT_UBUNTU_PNG, RES_T_PNG);
#endif
		if (!meta.empty() && !png.empty())
			font = std::make_unique<SudoFontMeta>(meta, png);
	});
	return font.get();
}

constexpr int kFallbackLineHeight = 30;
constexpr int kBoxAdvance = 16;

uint8_t Channel(uint32_t rgba, int shift) { return (uint8_t)(rgba >> shift); }

} // namespace

Canvas::Canvas(uint32_t width, uint32_t height)
{
	image.width = width;
	image.height = height;
	image.rgba.assign((size_t)width * height * 4, 0);
}

void Canvas::Clear(uint32_t rgba)
{
	for (size_t i = 0; i < image.rgba.size(); i += 4) {
		image.rgba[i] = Channel(rgba, 24);
		image.rgba[i + 1] = Channel(rgba, 16);
		image.rgba[i + 2] = Channel(rgba, 8);
		image.rgba[i + 3] = Channel(rgba, 0);
	}
}

void Canvas::Blend(int x, int y, uint32_t rgba, uint8_t coverage)
{
	if (x < 0 || y < 0 || x >= (int)image.width || y >= (int)image.height)
		return;
	uint8_t* p = &image.rgba[((size_t)y * image.width + x) * 4];
	const uint32_t a = Channel(rgba, 0) * coverage / 255;
	for (int c = 0; c < 3; c++)
		p[c] = (uint8_t)((Channel(rgba, 24 - 8 * c) * a + p[c] * (255 - a)) / 255);
	p[3] = (uint8_t)(a + p[3] * (255 - a) / 255);
}

void Canvas::FillRect(int x, int y, int w, int h, uint32_t rgba, int cornerRadius)
{
	const int r = std::max(0, std::min(cornerRadius, std::min(w, h) / 2));
	for (int iy = 0; iy < h; iy++) {
		for (int ix = 0; ix < w; ix++) {
			// Outside a corner's quarter circle: not drawn
			const int dx = ix < r ? r - ix : (ix >= w - r ? ix - (w - r - 1) : 0);
			const int dy = iy < r ? r - iy : (iy >= h - r ? iy - (h - r - 1) : 0);
			if (dx && dy && dx * dx + dy * dy > r * r)
				continue;
			Blend(x + ix, y + iy, rgba, 255);
		}
	}
}

void Canvas::Box(int x, int y, int w, int h, uint32_t rgba)
{
	for (int i = 0; i < w; i++) {
		Blend(x + i, y, rgba, 255);
		Blend(x + i, y + h - 1, rgba, 255);
	}
	for (int i = 0; i < h; i++) {
		Blend(x, y + i, rgba, 255);
		Blend(x + w - 1, y + i, rgba, 255);
	}
}

int Canvas::LineHeight()
{
	const SudoFontMeta* font = Font();
	return font ? (int)font->GetLineHeight() : kFallbackLineHeight;
}

int Canvas::TextWidth(std::wstring_view text) const
{
	const SudoFontMeta* font = Font();
	int width = 0;
	for (wchar_t ch : text) {
		const SudoFontMeta::CharInfo* info = font ? font->Find(ch) : nullptr;
		width += info ? info->XAdvance : kBoxAdvance;
	}
	return width;
}

void Canvas::Text(int x, int y, std::wstring_view text, uint32_t rgba)
{
	const SudoFontMeta* font = Font();
	const int line = LineHeight();
	for (wchar_t ch : text) {
		const SudoFontMeta::CharInfo* info = font ? font->Find(ch) : nullptr;
		if (!info) {
			Box(x + 2, y + line / 4, kBoxAdvance - 4, line / 2, rgba);
			x += kBoxAdvance;
			continue;
		}
		const std::vector<uint8_t>& atlas = font->Pixels();
		const int atlasWidth = (int)font->ImageWidth();
		for (int iy = 0; iy < info->PackedHeight; iy++) {
			for (int ix = 0; ix < info->PackedWidth; ix++) {
				const size_t at = ((size_t)(info->PackedY + iy) * atlasWidth + info->PackedX + ix) * 4;
				if (at + 3 >= atlas.size())
					continue;
				const uint8_t coverage = atlas[at + 3];
				if (coverage)
					Blend(x + info->XOffset + ix, y + info->YOffset + iy, rgba, coverage);
			}
		}
		x += info->XAdvance;
	}
}
