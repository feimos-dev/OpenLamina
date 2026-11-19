// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "Misc/Canvas.h"
#include "Misc/Image.h"
#include "../PngWriter.h"

#include <filesystem>
#include <fstream>

namespace {
// A 4x4 PNG of four quadrants (red, green / blue, white) at path
void WriteQuadrantsPng(const std::filesystem::path& path)
{
	std::vector<unsigned char> px(4 * 4 * 4);
	for (unsigned y = 0; y < 4; y++)
		for (unsigned x = 0; x < 4; x++) {
			const uint32_t c = y < 2 ? (x < 2 ? 0xff0000ff : 0x00ff00ff) : (x < 2 ? 0x0000ffff : 0xffffffff);
			unsigned char* p = &px[(y * 4 + x) * 4];
			p[0] = (unsigned char)(c >> 24), p[1] = (unsigned char)(c >> 16), p[2] = (unsigned char)(c >> 8), p[3] = (unsigned char)c;
		}
	std::vector<uint8_t> png = test_png::Encode(px.data(), 4, 4);
	std::filesystem::create_directories(path.parent_path());
	std::ofstream(path, std::ios::binary).write((const char*)png.data(), (std::streamsize)png.size());
}

std::filesystem::path Scratch(const char8_t* name)
{
	return std::filesystem::temp_directory_path() / "oc_canvas_tests" / std::filesystem::path(std::u8string(name));
}
} // namespace

TEST_CASE("Canvas: fill and blend")
{
	Canvas c(4, 4);
	c.Clear(0x000000ff);
	c.FillRect(0, 0, 4, 4, 0xffffff80);
	uint32_t px = c.Pixels().At(1, 1);
	const uint32_t red = px >> 24, alpha = px & 0xff;
	CHECK(red >= 0x7f);
	CHECK(red <= 0x81);
	CHECK(alpha == 0xff);
}

TEST_CASE("Canvas: rounded corners leave the corner pixel")
{
	Canvas c(20, 20);
	c.FillRect(0, 0, 20, 20, 0xffffffff, 8);
	CHECK(c.Pixels().At(0, 0) == 0);
	CHECK(c.Pixels().At(10, 10) == 0xffffffff);
}

TEST_CASE("Canvas: text draws only within its width and line")
{
	Canvas c(200, 60);
	c.Text(10, 10, L"Hi", 0xffffffff);
	const int width = c.TextWidth(L"Hi");
	CHECK(width > 0);
	bool any = false;
	for (uint32_t y = 0; y < 60; y++)
		for (uint32_t x = 0; x < 200; x++)
			if (c.Pixels().At(x, y)) {
				any = true;
				CHECK((int)x >= 10);
				CHECK((int)x < 10 + width + Canvas::LineHeight() / 2); // a glyph may reach past its advance
				CHECK((int)y >= 10);
				CHECK((int)y < 10 + Canvas::LineHeight());
			}
	CHECK(any);
}

TEST_CASE("Canvas: text off the edge doesn't write outside the image")
{
	Canvas c(8, 8);
	c.Text(-20, -20, L"WWWW", 0xffffffff);
	c.Text(6, 6, L"WWWW", 0xffffffff);
	CHECK(c.Pixels().rgba.size() == 8 * 8 * 4);
}

TEST_CASE("Image file: a PNG, also in a non-ASCII folder")
{
	auto path = Scratch(u8"Jürgen_テスト/q.png");
	WriteQuadrantsPng(path);
	std::string why;
	std::optional<Image> img = LoadImageFile(path, &why);
	REQUIRE(img);
	CHECK(img->width == 4);
	CHECK(img->At(0, 0) == 0xff0000ff);
	CHECK(img->At(3, 0) == 0x00ff00ff);
	CHECK(img->At(0, 3) == 0x0000ffff);
	CHECK(img->At(3, 3) == 0xffffffff);
}

TEST_CASE("Image file: rejects bad input")
{
	std::string why;
	CHECK_FALSE(LoadImageFile(Scratch(u8"missing.png"), &why));
	CHECK_FALSE(why.empty());

	// Truncated
	auto good = Scratch(u8"good.png");
	WriteQuadrantsPng(good);
	std::ifstream in(good, std::ios::binary);
	std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	auto cut = Scratch(u8"cut.png");
	std::ofstream(cut, std::ios::binary).write(bytes.data(), (std::streamsize)(bytes.size() / 2));
	CHECK_FALSE(LoadImageFile(cut, &why));

	// Not an image
	auto text = Scratch(u8"text.png");
	std::ofstream(text) << "not an image";
	CHECK_FALSE(LoadImageFile(text, &why));

	// A JPEG is named as one
	auto jpeg = Scratch(u8"photo.jpg");
	{
		const unsigned char header[] = { 0xFF, 0xD8, 0xFF, 0xE0, 0, 16, 'J', 'F', 'I', 'F', 0 };
		std::ofstream(jpeg, std::ios::binary).write((const char*)header, (std::streamsize)sizeof(header));
	}
	why.clear();
	CHECK_FALSE(LoadImageFile(jpeg, &why));
	CHECK(why.find("JPEG") != std::string::npos);

	// 16384 pixels wide: refused from its header, without decoding
	std::vector<uint8_t> row(16384 * 4, 0);
	std::vector<uint8_t> png = test_png::Encode(row.data(), 16384, 1);
	auto wide = Scratch(u8"wide.png");
	std::ofstream(wide, std::ios::binary).write((const char*)png.data(), (std::streamsize)png.size());
	CHECK_FALSE(LoadImageFile(wide, &why));

	// More than 64 MiB: refused by its size, without being read
	auto huge = Scratch(u8"huge.png");
	{
		std::ofstream out(huge, std::ios::binary);
		out.write(bytes.data(), (std::streamsize)bytes.size());
		out.seekp((std::streamoff)kMaxImageFileBytes);
		out.put('\0');
	}
	why.clear();
	CHECK_FALSE(LoadImageFile(huge, &why));
	CHECK(why.find("64 MiB") != std::string::npos);
}
