// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

#include "Misc/Canvas.h"
#include "Misc/Keyboard/KeyboardLayout.h"
#include "Misc/Keyboard/SudoFontMeta.h"
#include "Misc/Keyboard/VRKeyboard.h"
#include "Misc/LaserMouse.h"
#include "Reimpl/BaseOverlay.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

const KeyboardLayout* Us()
{
	return KeyboardLayout::Get(KeyboardLayout::Name::US);
}

std::vector<vr::VREvent_t> OfType(const std::vector<vr::VREvent_t>& events, uint32_t type)
{
	std::vector<vr::VREvent_t> out;
	for (const vr::VREvent_t& e : events)
		if (e.eventType == type)
			out.push_back(e);
	return out;
}

Image Drawn(const VRKeyboard& kb)
{
	Canvas canvas(kb.CanvasWidth(), kb.CanvasHeight());
	kb.Draw(canvas);
	return canvas.Pixels();
}

// A key of a layout by its character
const KeyboardLayout::Key* KeyFor(const KeyboardLayout& layout, wchar_t ch)
{
	for (const KeyboardLayout::Key& key : layout.GetKeymap())
		if (key.ch == ch)
			return &key;
	return nullptr;
}

} // namespace

TEST_CASE("Typing_builds_text_and_events")
{
	REQUIRE(Us());
	VRKeyboard kb(VRKeyboard::Mode::Normal, false, 0, "", 0, 42, Us());
	kb.Type(U'H');
	kb.Type(U'i');
	CHECK(kb.Text() == "Hi");
	std::vector<vr::VREvent_t> events = kb.TakeEvents();
	std::vector<vr::VREvent_t> chars = OfType(events, vr::VREvent_KeyboardCharInput);
	REQUIRE(chars.size() == 2);
	CHECK(std::string(chars[0].data.keyboard.cNewInput) == "H");
	CHECK(std::string(chars[1].data.keyboard.cNewInput) == "i");
	CHECK(chars[0].data.keyboard.uUserValue == 42);
	// Opened first, then a character event per key
	REQUIRE(events.size() == 3);
	CHECK(events[0].eventType == vr::VREvent_KeyboardOpened_Global);
	CHECK(kb.TakeEvents().empty());

	// Backspace, a character of more than one byte, and Done
	kb.Type(U'\b');
	kb.Type(U'\xA3');
	CHECK(kb.Text() == "H\xC2\xA3");
	kb.Type(VRKeyboard::kDone);
	events = kb.TakeEvents();
	REQUIRE(events.size() == 5);
	CHECK(std::string(events[0].data.keyboard.cNewInput) == "\b");
	CHECK(std::string(events[1].data.keyboard.cNewInput) == "\xC2\xA3");
	CHECK(events[2].eventType == vr::VREvent_KeyboardDone);
	CHECK(events[3].eventType == vr::VREvent_KeyboardClosed);
	CHECK(events[4].eventType == vr::VREvent_KeyboardClosed_Global);
	CHECK(kb.Done());
	CHECK(kb.Closed());
}

TEST_CASE("Max_chars_respected")
{
	VRKeyboard kb(VRKeyboard::Mode::Normal, false, 0, "ab", 3, 0, Us());
	kb.Type(U'c');
	kb.Type(U'd');
	CHECK(kb.Text() == "abc");
	CHECK(OfType(kb.TakeEvents(), vr::VREvent_KeyboardCharInput).size() == 1);
	// A starting text past the limit is cut to it
	VRKeyboard longer(VRKeyboard::Mode::Normal, false, 0, "abcdef", 4, 0, Us());
	CHECK(longer.Text() == "abcd");
}

TEST_CASE("Password_draws_stars")
{
	REQUIRE(Us());
	VRKeyboard password(VRKeyboard::Mode::Password, false, 0, "Hi", 0, 0, Us());
	VRKeyboard stars(VRKeyboard::Mode::Normal, false, 0, "**", 0, 0, Us());
	VRKeyboard plain(VRKeyboard::Mode::Normal, false, 0, "Hi", 0, 0, Us());
	CHECK(password.Text() == "Hi");
	// Drawn as the stars would be: no glyph of the text
	CHECK(Drawn(password).rgba == Drawn(stars).rgba);
	// Where the font is built in, the text itself looks different
	Canvas h(40, 40), star(40, 40);
	h.Text(0, 0, L"H", 0xffffffff);
	star.Text(0, 0, L"*", 0xffffffff);
	if (h.Pixels().rgba != star.Pixels().rgba)
		CHECK(Drawn(password).rgba != Drawn(plain).rgba);
}

TEST_CASE("Minimal_sends_without_text")
{
	VRKeyboard kb(VRKeyboard::Mode::Minimal, false, VRKeyboard::kFlagMinimal, "", 0, 7, Us());
	kb.Type(U'a');
	kb.Type(U'b');
	CHECK(kb.Text().empty());
	std::vector<vr::VREvent_t> chars = OfType(kb.TakeEvents(), vr::VREvent_KeyboardCharInput);
	REQUIRE(chars.size() == 2);
	CHECK(std::string(chars[0].data.keyboard.cNewInput) == "a");
	CHECK(std::string(chars[1].data.keyboard.cNewInput) == "b");
	// No text box: shorter than the normal keyboard
	VRKeyboard normal(VRKeyboard::Mode::Normal, false, 0, "", 0, 0, Us());
	CHECK(kb.CanvasHeight() < normal.CanvasHeight());
}

TEST_CASE("Us_layout_has_at_above_2")
{
	const KeyboardLayout* us = Us();
	REQUIRE(us);
	const KeyboardLayout::Key* two = KeyFor(*us, L'2');
	REQUIRE(two);
	CHECK(two->shift == L'@');
	const KeyboardLayout* uk = KeyboardLayout::Get(KeyboardLayout::Name::UK);
	REQUIRE(uk);
	two = KeyFor(*uk, L'2');
	REQUIRE(two);
	CHECK(two->shift == L'"');
	CHECK(KeyboardLayout::ForLocale("en-GB") == KeyboardLayout::Name::UK);
	CHECK(KeyboardLayout::ForLocale("en_GB.UTF-8") == KeyboardLayout::Name::UK);
	CHECK(KeyboardLayout::ForLocale("en-US") == KeyboardLayout::Name::US);
	CHECK(KeyboardLayout::ForLocale("") == KeyboardLayout::Name::US);
}

TEST_CASE("Font_data_that_is_cut_short_gives_a_font_without_characters")
{
	const std::string header = "\x0bSudoFont1.1";
	// A header alone, a section that claims more bytes than there are, and nothing at all
	for (std::vector<char> data : { std::vector<char>(header.begin(), header.end()),
	         std::vector<char>{ header.begin(), header.end() }, std::vector<char>{} }) {
		if (data.size() == header.size()) {
			data.push_back(1); // a section id cut short
		}
		SudoFontMeta font(data, std::vector<char>{});
		CHECK(font.Find(L'A') == nullptr);
		CHECK(font.ImageWidth() == 0);
	}
	std::vector<char> big(header.begin(), header.end());
	for (char c : { 0, 0, 100, 0, 0, 0 }) // font info section of 100 bytes, none there
		big.push_back(c);
	SudoFontMeta font(big, std::vector<char>{});
	CHECK(font.Find(L'A') == nullptr);
}

TEST_CASE("Wide_and_UTF8_strings_convert_both_ways")
{
	CHECK(VRKeyboard::WideToUtf8(L"a\xA3") == "a\xC2\xA3");
	CHECK(VRKeyboard::Utf8ToWide("a\xC2\xA3") == L"a\xA3");
	CHECK(VRKeyboard::Utf8ToWide(VRKeyboard::WideToUtf8(L"x€y")) == L"x€y");
	CHECK(VRKeyboard::WideToUtf8(L"").empty());
}

TEST_CASE("Caret_inserts_and_deletes_where_it_is")
{
	REQUIRE(Us());
	VRKeyboard kb(VRKeyboard::Mode::Normal, false, 0, "ac", 0, 0, Us());
	CHECK(kb.Caret() == 2);
	kb.PressArrow(VRKeyboard::Left);
	kb.Type(U'b');
	CHECK(kb.Text() == "abc");
	CHECK(kb.Caret() == 2);
	kb.Type(U'\b');
	kb.Type(U'\b');
	CHECK(kb.Text() == "c");
	kb.Type(U'\b'); // at the start: nothing to delete
	CHECK(kb.Text() == "c");
	kb.PressArrow(VRKeyboard::Down); // a single line: to its end
	CHECK(kb.Caret() == 1);
	kb.PressArrow(VRKeyboard::Right);
	CHECK(kb.Caret() == 1);
	kb.PressArrow(VRKeyboard::Up);
	CHECK(kb.Caret() == 0);
}

TEST_CASE("Arrows_move_between_lines_of_a_multiline_text")
{
	REQUIRE(Us());
	VRKeyboard kb(VRKeyboard::Mode::Normal, true, 0, "abc\nd\nefg", 0, 0, Us());
	CHECK(kb.Caret() == 9);
	kb.PressArrow(VRKeyboard::Up); // column 3 of the last line, on a line of one character
	CHECK(kb.Caret() == 5);
	kb.PressArrow(VRKeyboard::Up);
	CHECK(kb.Caret() == 1);
	kb.PressArrow(VRKeyboard::Down);
	CHECK(kb.Caret() == 5);
}

TEST_CASE("A_minimal_keyboard_sends_the_escape_sequence_of_an_arrow")
{
	REQUIRE(Us());
	VRKeyboard kb(VRKeyboard::Mode::Minimal, false, VRKeyboard::kFlagMinimal | VRKeyboard::kFlagShowArrowKeys, "", 0, 0, Us());
	kb.TakeEvents();
	kb.PressArrow(VRKeyboard::Left);
	std::vector<vr::VREvent_t> chars = OfType(kb.TakeEvents(), vr::VREvent_KeyboardCharInput);
	REQUIRE(chars.size() == 1);
	CHECK(std::string(chars[0].data.keyboard.cNewInput) == "\x1b[D");
}

TEST_CASE("Done_stays_open_for_a_chat_line_and_hide_only_closes")
{
	REQUIRE(Us());
	VRKeyboard chat(VRKeyboard::Mode::Submit, false, 0, "", 0, 0, Us());
	chat.TakeEvents();
	chat.Type(VRKeyboard::kDone);
	CHECK_FALSE(chat.Closed());
	CHECK(OfType(chat.TakeEvents(), vr::VREvent_KeyboardClosed).empty());

	VRKeyboard kb(VRKeyboard::Mode::Normal, false, VRKeyboard::kFlagHideDoneKey, "", 0, 0, Us());
	kb.TakeEvents();
	kb.Close();
	std::vector<vr::VREvent_t> events = kb.TakeEvents();
	CHECK(OfType(events, vr::VREvent_KeyboardDone).empty());
	CHECK(OfType(events, vr::VREvent_KeyboardClosed).size() == 1);
	CHECK(OfType(events, vr::VREvent_KeyboardClosed_Global).size() == 1);
	CHECK(kb.Closed());
}

TEST_CASE("Text_without_a_keyboard_keeps_what_was_given")
{
	CHECK(VRKeyboard::TextWithoutKeyboard("Bob") == "Bob");
	CHECK(VRKeyboard::TextWithoutKeyboard("") == "Adventurer");
}

TEST_CASE("Arrow_keys_add_a_row_and_a_minimal_keyboard_shows_them_only_when_asked")
{
	REQUIRE(Us());
	VRKeyboard plain(VRKeyboard::Mode::Minimal, false, VRKeyboard::kFlagMinimal, "", 0, 0, Us());
	VRKeyboard arrows(VRKeyboard::Mode::Minimal, false, VRKeyboard::kFlagMinimal | VRKeyboard::kFlagShowArrowKeys, "", 0, 0, Us());
	CHECK(arrows.CanvasHeight() > plain.CanvasHeight());
}

TEST_CASE("Keyboard_keeps_its_size_so_its_keys_stay_where_they_were")
{
	REQUIRE(Us());
	VRKeyboard kb(VRKeyboard::Mode::Normal, false, 0, "", 0, 0, Us());
	CHECK(kb.CanvasWidth() == 1000);
	CHECK(kb.CanvasHeight() == 404);
	VRKeyboard minimal(VRKeyboard::Mode::Minimal, false, VRKeyboard::kFlagMinimal, "", 0, 0, Us());
	CHECK(minimal.CanvasHeight() < 404);
}

namespace {
// The laser on the keyboard at (x, y) on its canvas
void Point(VRKeyboard& kb, uint32_t type, int x, int y, bool held)
{
	vr::VREvent_t e{};
	e.eventType = type;
	e.data.mouse.x = (float)x;
	e.data.mouse.y = (float)y;
	e.data.mouse.button = held ? vr::VRMouseButton_Left : 0;
	LaserMouse mouse;
	kb.OnPointer(mouse, { PointerEvent{ kb.Handle(), e } });
}
} // namespace

TEST_CASE("Dragging_across_the_text_selects_and_typing_replaces_it")
{
	REQUIRE(Us());
	VRKeyboard kb(VRKeyboard::Mode::Normal, false, 0, "hello world", 0, 0, Us());
	Drawn(kb);
	kb.TakeEvents();
	const int y = 20; // in the text box, which is at the top
	// A click at the box's left edge puts the caret at the start
	Point(kb, vr::VREvent_MouseButtonDown, 10, y, true);
	CHECK(kb.Caret() == 0);
	CHECK(kb.Selection().first == kb.Selection().second);
	// A drag to the right selects everything (the box ends well past the text)
	Point(kb, vr::VREvent_MouseMove, 600, y, true);
	Point(kb, vr::VREvent_MouseButtonUp, 600, y, false);
	CHECK(kb.Selection() == std::make_pair<size_t, size_t>(0, 11));
	// Moving after the release selects no more
	Point(kb, vr::VREvent_MouseMove, 10, y, false);
	CHECK(kb.Selection() == std::make_pair<size_t, size_t>(0, 11));

	kb.Type(U'x');
	CHECK(kb.Text() == "x");
	std::vector<vr::VREvent_t> input = OfType(kb.TakeEvents(), vr::VREvent_KeyboardCharInput);
	REQUIRE(input.size() == 12); // a backspace for each character, then the one typed
	CHECK(std::string(input[0].data.keyboard.cNewInput) == "\b");
	CHECK(std::string(input[11].data.keyboard.cNewInput) == "x");

	// A selection made in code goes with a backspace, and an arrow drops it
	kb.Type(U'y');
	kb.Select(0, 1);
	kb.Type(U'\b');
	CHECK(kb.Text() == "y");
	kb.Select(1, 0);
	kb.PressArrow(VRKeyboard::Right); // to the selection's end
	CHECK(kb.Selection().first == kb.Selection().second);
	CHECK(kb.Caret() == 1);
	kb.Select(1, 0);
	kb.PressArrow(VRKeyboard::Left); // to its start
	CHECK(kb.Caret() == 0);
}

TEST_CASE("A_long_text_scrolls_only_as_the_caret_leaves_the_view")
{
	REQUIRE(Us());
	VRKeyboard kb(VRKeyboard::Mode::Normal, false, 0, std::string(300, 'm'), 0, 0, Us());
	Drawn(kb);
	const size_t atEnd = kb.ViewStart();
	CHECK(atEnd > 0); // the end is shown, with the caret
	// A step back stays in view: the text doesn't move
	kb.PressArrow(VRKeyboard::Left);
	Drawn(kb);
	CHECK(kb.ViewStart() == atEnd);
	// To the start: the view follows, and stays there as the caret moves right
	kb.PressArrow(VRKeyboard::Up);
	Drawn(kb);
	CHECK(kb.ViewStart() == 0);
	kb.PressArrow(VRKeyboard::Right);
	kb.PressArrow(VRKeyboard::Right);
	Drawn(kb);
	CHECK(kb.ViewStart() == 0);
	// A click lands on the character shown there, not one counted from the text's start
	kb.PressArrow(VRKeyboard::Down);
	Drawn(kb);
	Point(kb, vr::VREvent_MouseButtonDown, 10, 20, true);
	CHECK(kb.Caret() == kb.ViewStart());
}

TEST_CASE("A_multiline_text_shows_the_caret_s_line")
{
	REQUIRE(Us());
	VRKeyboard kb(VRKeyboard::Mode::Normal, true, 0, "first line\nsecond", 0, 0, Us());
	Drawn(kb);
	CHECK(kb.ViewStart() == 11);
	Point(kb, vr::VREvent_MouseButtonDown, 10, 20, true);
	CHECK(kb.Caret() == 11); // the start of the line shown
	kb.PressArrow(VRKeyboard::Up);
	Drawn(kb);
	CHECK(kb.ViewStart() == 0);
	Point(kb, vr::VREvent_MouseButtonDown, 600, 20, true);
	CHECK(kb.Caret() == 10); // its end, before the line break
}

TEST_CASE("Text_wraps_between_words_in_at_most_the_lines_asked")
{
	Canvas canvas(1, 1);
	const int width = canvas.TextWidth(L"hello world");
	std::vector<std::wstring> lines = WrapText(canvas, L"hello world hello world", width, 3);
	REQUIRE(lines.size() == 2);
	CHECK(lines[0] == L"hello world");
	CHECK(lines[1] == L"hello world");
	CHECK(WrapText(canvas, L"one\ntwo", 1000, 3) == std::vector<std::wstring>{ L"one", L"two" });
	CHECK(WrapText(canvas, L"a b c d", canvas.TextWidth(L"a"), 2).size() == 2);
	// A word wider than a line is cut
	lines = WrapText(canvas, L"abcdefgh", canvas.TextWidth(L"abcd"), 3);
	REQUIRE(lines.size() == 2);
	CHECK(lines[0] == L"abcd");
}
