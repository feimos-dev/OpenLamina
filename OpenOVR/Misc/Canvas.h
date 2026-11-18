#pragma once
// Drawing on the CPU, into an RGBA image that a swapchain filled from memory (CpuSwapchain) then shows: what
// OpenLamina draws itself (the keyboard, message boxes, the dashboard, cursors, bounds).

#include "Image.h"

#include <cstdint>
#include <string_view>

class Canvas {
public:
	Canvas(uint32_t width, uint32_t height);

	void Clear(uint32_t rgba);
	// Alpha-blended; cornerRadius rounds the corners
	void FillRect(int x, int y, int w, int h, uint32_t rgba, int cornerRadius = 0);
	// In the keyboard's font (its one size, LineHeight() tall), with (x, y) the top-left of the line. A character the
	// font lacks is drawn as an empty box. Without the font (where it isn't built in), boxes only.
	void Text(int x, int y, std::wstring_view text, uint32_t rgba);
	int TextWidth(std::wstring_view text) const;
	static int LineHeight();
	// Scaled into the rectangle (nearest pixel)
	void Draw(const Image& image, int x, int y, int w, int h);

	const Image& Pixels() const { return image; }

private:
	void Blend(int x, int y, uint32_t rgba, uint8_t coverage);
	void Box(int x, int y, int w, int h, uint32_t rgba);

	Image image;
};
