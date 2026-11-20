#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

class SudoFontMeta {
public:
	SudoFontMeta(std::vector<char> data, std::vector<char> image);
	~SudoFontMeta();

	struct CharInfo {
		// Which character this Character references.
		wchar_t CharacterCode;

		// Location of this character in the packed image.
		short PackedX;
		short PackedY;
		short PackedWidth;
		short PackedHeight;

		// Where to draw this character on the target.
		short XOffset;
		short YOffset;

		// How much to advance your X position after drawing this character.
		// The total amount to advance for each char is ( XAdvance + GetKerning( nextChar ) ).
		short XAdvance;
	};

	unsigned int GetLineHeight() const { return lineHeight; }

	// For drawing glyphs from the packed image: a character's place in it (null if the font lacks it), the image
	// (RGBA8, rows of ImageWidth() pixels)
	const CharInfo* Find(wchar_t ch) const
	{
		auto it = chars.find(ch);
		return it == chars.end() ? nullptr : &it->second;
	}
	const std::vector<uint8_t>& Pixels() const { return pixel_data; }
	unsigned int ImageWidth() const { return imgWidth; }

private:
	std::map<wchar_t, CharInfo> chars;
	std::vector<uint8_t> pixel_data;

	unsigned int imgWidth;
	unsigned int imgHeight;

	unsigned int lineHeight;
};
