#include "stdafx.h"

#include "SudoFontMeta.h"

#include "Misc/lodepng.h"

#include <cstring>

namespace {

// Reads little-endian values off the front of a buffer; a read past its end fails the reader (and gives zero)
class Reader {
public:
	explicit Reader(std::vector<char>& data)
	    : data(data)
	{
	}

	bool Ok() const { return ok; }

	std::vector<char> Bytes(size_t len)
	{
		if (len > data.size() - pos) {
			ok = false;
			return {};
		}
		std::vector<char> result(data.begin() + (std::ptrdiff_t)pos, data.begin() + (std::ptrdiff_t)(pos + len));
		pos += len;
		return result;
	}

	template <typename T>
	T Read()
	{
		T val{};
		if (sizeof(val) > data.size() - pos) {
			ok = false;
			return val;
		}
		memcpy(&val, data.data() + pos, sizeof(val));
		pos += sizeof(val);
		return val;
	}

private:
	std::vector<char>& data;
	size_t pos = 0;
	bool ok = true;
};

} // namespace

enum SectionID_t : uint16_t {
	ID_FONT_INFO = 0,
	ID_CHARACTERS,
	ID_KERNING,
	ID_CONFIG,

	ID_END = 999,
};

SudoFontMeta::SudoFontMeta(std::vector<char> data, std::vector<char> image)
{
	// A font that can't be read has no characters: every one is drawn as an empty box
	auto fail = [&](const char* why) {
		OOVR_LOGF("The built-in font can't be read: %s", why);
		chars.clear();
		pixel_data.clear();
		imgWidth = imgHeight = 0;
	};
	imgWidth = imgHeight = 0;
	lineHeight = 0;

	const std::string headerReq = "\x0bSudoFont1.1";
	Reader in(data);
	std::vector<char> headerVec = in.Bytes(headerReq.length());
	if (!in.Ok() || std::string(headerVec.data(), headerVec.size()) != headerReq)
		{ fail("bad header"); return; }

	unsigned int origTexW = 0, origTexH = 0;

	while (true) {
		auto sectionID = in.Read<SectionID_t>();
		if (!in.Ok())
			{ fail("truncated"); return; }

		if (sectionID == ID_END)
			break;

		auto sectionSize = in.Read<uint32_t>();
		std::vector<char> sec = in.Bytes(sectionSize);
		if (!in.Ok())
			{ fail("truncated section"); return; }
		Reader r(sec);

		if (sectionID == ID_FONT_INFO) {
			lineHeight = r.Read<uint16_t>();
			origTexH = r.Read<uint16_t>();
			origTexW = r.Read<uint16_t>();
		} else if (sectionID == ID_CHARACTERS) {
			auto count = r.Read<uint16_t>();

			for (size_t i = 0; i < count && r.Ok(); i++) {
				CharInfo info = { 0 };

				info.CharacterCode = r.Read<uint16_t>();

				info.PackedX = r.Read<uint16_t>();
				info.PackedY = r.Read<uint16_t>();
				info.PackedWidth = r.Read<uint16_t>();
				info.PackedHeight = r.Read<uint16_t>();

				info.XOffset = r.Read<uint16_t>();
				info.YOffset = r.Read<uint16_t>();

				info.XAdvance = r.Read<uint16_t>();

				chars[info.CharacterCode] = info;
			}
		}
		if (!r.Ok())
			{ fail("truncated section"); return; }
	}

	// Load the image itself
	if (lodepng::decode(
	        pixel_data,
	        imgWidth, imgHeight,
	        (const uint8_t*)image.data(), image.size(),
	        LCT_RGBA, 8)
	    != 0)
		{ fail("its image can't be decoded"); return; }
	if (origTexW != imgWidth || origTexH != imgHeight)
		{ fail("its image isn't the size it names"); return; }

	// A glyph outside the image is never drawn from it
	for (auto it = chars.begin(); it != chars.end();) {
		const CharInfo& c = it->second;
		const bool inside = c.PackedX >= 0 && c.PackedY >= 0 && c.PackedWidth >= 0 && c.PackedHeight >= 0
		    && (unsigned)c.PackedX + (unsigned)c.PackedWidth <= imgWidth && (unsigned)c.PackedY + (unsigned)c.PackedHeight <= imgHeight;
		it = inside ? std::next(it) : chars.erase(it);
	}
}

SudoFontMeta::~SudoFontMeta()
{
}
