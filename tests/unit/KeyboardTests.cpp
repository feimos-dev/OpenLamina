// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

#include "Misc/Canvas.h"
#include "Misc/Keyboard/KeyboardLayout.h"
#include "Misc/Keyboard/VRKeyboard.h"
#include "Misc/LaserMouse.h"

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
	CHECK(events.size() == 2);
	CHECK(kb.TakeEvents().empty());

	// Backspace, a character of more than one byte, and Done
	kb.Type(U'\b');
	kb.Type(U'\xA3');
	CHECK(kb.Text() == "H\xC2\xA3");
	kb.Type(VRKeyboard::kDone);
	events = kb.TakeEvents();
	REQUIRE(events.size() == 3);
	CHECK(std::string(events[0].data.keyboard.cNewInput) == "\b");
	CHECK(std::string(events[1].data.keyboard.cNewInput) == "\xC2\xA3");
	CHECK(events[2].eventType == vr::VREvent_KeyboardDone);
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

TEST_CASE("Keyboard_typed_with_the_laser")
{
	REQUIRE(Us());
	VRKeyboard kb(VRKeyboard::Mode::Normal, false, 0, "", 0, 0, Us());
	kb.SetPlacement(XrPosef{ { 0, 0, 0, 1 }, { 0, 0, -1 } });
	PointerTarget target{};
	target.handle = kb.Handle();
	target.placement = overlay_maths::Placement{ kb.Placement(), kb.WidthMeters(), kb.HeightMeters(), 0 };
	target.mouseScale = { { (float)kb.CanvasWidth(), (float)kb.CanvasHeight() } };
	LaserMouse mouse;
	// Each key's centre: a click there types it
	auto click = [&](wchar_t ch) {
		const KeyboardLayout::Key* key = KeyFor(*Us(), ch);
		REQUIRE(key);
		// The same geometry the keyboard draws with: a key pitch of (width - padding) / layout width
		const float pitch = (float)((kb.CanvasWidth() - 8) / Us()->GetWidth());
		const float keysTop = 8.0f + pitch; // below the text box
		const float px = 8 + pitch * key->x + (pitch - 8) / 2;
		const float py = keysTop + pitch * key->y + (pitch - 8) / 2;
		const float x = (px / kb.CanvasWidth() - 0.5f) * kb.WidthMeters();
		const float y = (0.5f - py / kb.CanvasHeight()) * kb.HeightMeters();
		for (bool down : { false, true, false }) {
			PointerRay ray{};
			ray.hand = 1;
			ray.origin = { x, y, 0 };
			ray.dir = { 0, 0, -1 };
			ray.click = down;
			ray.valid = true;
			std::vector<PointerEvent> events;
			mouse.Update({ PointerRay{}, ray }, { target }, 0, events);
			kb.OnPointer(mouse, events);
		}
	};
	click(L'h');
	click(L'i');
	CHECK(kb.Text() == "hi");
	CHECK(OfType(kb.TakeEvents(), vr::VREvent_KeyboardCharInput).size() == 2);
}
