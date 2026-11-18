#include "stdafx.h"

#include "VRKeyboard.h"

#include "Misc/Canvas.h"
#include "Misc/LaserMouse.h"

#include <algorithm>
#include <cstring>

std::wstring_convert<std::codecvt_utf8<wchar_t>> VRKeyboard::CHAR_CONV;

namespace {

// The layout's special keys (KeyboardLayout's escapes)
constexpr wchar_t kShiftKey = 1, kCapsKey = 2, kDoneKey = 3;

// Colours, 0xRRGGBBAA
constexpr uint32_t kBackground = 0x2b2f36f0;
constexpr uint32_t kKey = 0x4a505cff;
constexpr uint32_t kKeyHovered = 0x6a7486ff;
constexpr uint32_t kKeyLit = 0xe8e8e8ff; // shift or caps on
constexpr uint32_t kLabel = 0xffffffff;
constexpr uint32_t kLabelLit = 0x000000ff;
constexpr uint32_t kTextBox = 0xffffffff;
constexpr uint32_t kText = 0x000000ff;
constexpr uint32_t kClose = 0xb04040ff;
constexpr uint32_t kCloseHovered = 0xd05a5aff;

std::string Utf8(char32_t c)
{
	std::string out;
	if (c < 0x80) {
		out += (char)c;
	} else if (c < 0x800) {
		out += (char)(0xc0 | (c >> 6));
		out += (char)(0x80 | (c & 0x3f));
	} else if (c < 0x10000) {
		out += (char)(0xe0 | (c >> 12));
		out += (char)(0x80 | ((c >> 6) & 0x3f));
		out += (char)(0x80 | (c & 0x3f));
	} else {
		out += (char)(0xf0 | (c >> 18));
		out += (char)(0x80 | ((c >> 12) & 0x3f));
		out += (char)(0x80 | ((c >> 6) & 0x3f));
		out += (char)(0x80 | (c & 0x3f));
	}
	return out;
}

std::u32string FromUtf8(const std::string& in)
{
	std::u32string out;
	for (size_t i = 0; i < in.size();) {
		const unsigned char b = (unsigned char)in[i];
		int extra = b < 0x80 ? 0 : (b >> 5) == 6 ? 1 : (b >> 4) == 14 ? 2 : (b >> 3) == 30 ? 3 : -1;
		if (extra < 0 || i + (size_t)extra >= in.size()) {
			i++; // not UTF-8: skipped
			continue;
		}
		char32_t c = extra == 0 ? b : (char32_t)(b & (0x3f >> extra));
		bool ok = true;
		for (int k = 1; k <= extra; k++) {
			const unsigned char next = (unsigned char)in[i + k];
			if ((next & 0xc0) != 0x80) {
				ok = false;
				break;
			}
			c = (c << 6) | (next & 0x3f);
		}
		i += ok ? extra + 1 : 1;
		if (ok)
			out += c;
	}
	return out;
}

std::wstring Wide(const std::u32string& s)
{
	std::wstring out;
	for (char32_t c : s)
		out += (wchar_t)(c <= 0xffff ? c : 0xfffd); // the font has nothing beyond 16 bits
	return out;
}

} // namespace

VRKeyboard::VRKeyboard(Mode mode, bool multiline, uint32_t flags, std::string initialText, uint32_t maxChars, uint64_t userValue)
    : VRKeyboard(mode, multiline, flags, std::move(initialText), maxChars, userValue, KeyboardLayout::Current())
{
}

VRKeyboard::VRKeyboard(Mode mode, bool multiline, uint32_t flags, std::string initialText, uint32_t maxChars, uint64_t userValue, const KeyboardLayout* layout)
    : mode(mode), multiline(multiline), flags(flags), maxChars(maxChars), userValue(userValue), layout(layout)
{
	if (mode != Mode::Minimal) {
		chars = FromUtf8(initialText);
		if (maxChars && chars.size() > maxChars)
			chars.resize(maxChars);
		for (char32_t c : chars)
			text += Utf8(c);
	}
}

std::vector<vr::VREvent_t> VRKeyboard::TakeEvents()
{
	std::vector<vr::VREvent_t> out;
	out.swap(events);
	return out;
}

void VRKeyboard::Send(vr::EVREventType type, const std::string& utf8)
{
	vr::VREvent_t e{};
	e.eventType = type;
	e.trackedDeviceIndex = 0; // as SteamVR sends them
	e.data.keyboard.uUserValue = userValue;
	// Seven bytes and the terminator
	memcpy(e.data.keyboard.cNewInput, utf8.data(), std::min<size_t>(utf8.size(), sizeof(e.data.keyboard.cNewInput) - 1));
	events.push_back(e);
}

void VRKeyboard::Type(char32_t c)
{
	if (closed)
		return;
	if (c == kDone || (c == U'\n' && !multiline)) {
		Send(vr::VREvent_KeyboardDone);
		done = true;
		// A chat line stays open for the next one
		if (mode != Mode::Submit)
			closed = true;
		dirty = true;
		return;
	}
	if (c == U'\t' && !multiline)
		return;
	if (c == U'\b') {
		if (mode != Mode::Minimal && !chars.empty()) {
			chars.pop_back();
			text.clear();
			for (char32_t k : chars)
				text += Utf8(k);
		}
		Send(vr::VREvent_KeyboardCharInput, "\b");
		dirty = true;
		return;
	}
	if (c < 0x20 && c != U'\n' && c != U'\t')
		return; // the layout's other controls type nothing
	if (mode != Mode::Minimal) {
		if (maxChars && chars.size() >= maxChars)
			return;
		chars += c;
		text += Utf8(c);
	}
	Send(vr::VREvent_KeyboardCharInput, Utf8(c));
	dirty = true;
}

void VRKeyboard::Close()
{
	if (closed)
		return;
	closed = true;
	Send(vr::VREvent_KeyboardClosed);
	dirty = true;
}

void VRKeyboard::Press(const KeyboardLayout::Key& key)
{
	const wchar_t ch = caseMode == Case::Lower ? key.ch : key.shift;
	if (ch == kShiftKey || ch == kCapsKey) {
		const Case target = ch == kCapsKey ? Case::Lock : Case::Shift;
		caseMode = caseMode == target ? Case::Lower : target;
		dirty = true;
		return;
	}
	if (ch == kDoneKey) {
		Type(kDone);
		return;
	}
	Type((char32_t)ch);
	// Shift lasts one character
	if (caseMode == Case::Shift && ch != L'\b') {
		caseMode = Case::Lower;
		dirty = true;
	}
}

int VRKeyboard::KeySize() const
{
	const int width = layout ? std::max(1, layout->GetWidth()) : 15;
	return (kWidth - kPadding) / width - kPadding;
}

int VRKeyboard::KeysTop() const
{
	// Below the text box, which a minimal keyboard hasn't
	return mode == Mode::Minimal ? kPadding : kPadding + Pitch();
}

int VRKeyboard::CanvasHeight() const
{
	const float rows = layout ? std::max(1.0f, layout->GetHeight()) : 5.0f;
	return KeysTop() + (int)(Pitch() * rows);
}

VRKeyboard::Rect VRKeyboard::KeyRect(const KeyboardLayout::Key& key) const
{
	Rect r;
	r.x = kPadding + (int)(Pitch() * key.x);
	r.y = KeysTop() + (int)(Pitch() * key.y);
	r.w = (int)(Pitch() * key.w) - kPadding;
	r.h = (int)(Pitch() * key.h) - kPadding;
	if (key.spansToRight)
		r.w = kWidth - kPadding - r.x;
	return r;
}

VRKeyboard::Rect VRKeyboard::TextRect() const
{
	return Rect{ kPadding, kPadding, kWidth - 3 * kPadding - KeySize(), KeySize() };
}

VRKeyboard::Rect VRKeyboard::CloseRect() const
{
	return Rect{ kWidth - kPadding - KeySize(), kPadding, KeySize(), KeySize() };
}

int VRKeyboard::KeyAt(int x, int y) const
{
	if (!layout)
		return -1;
	const KeyboardLayout::keymap_t& keys = layout->GetKeymap();
	for (size_t i = 0; i < keys.size(); i++)
		if (KeyRect(keys[i]).Contains(x, y))
			return (int)i;
	return -1;
}

void VRKeyboard::OnPointer(const LaserMouse& mouse, const std::vector<PointerEvent>& pointer)
{
	(void)mouse;
	for (const PointerEvent& p : pointer) {
		if (p.handle != Handle())
			continue;
		const vr::VREvent_t& e = p.event;
		const int x = (int)e.data.mouse.x, y = (int)e.data.mouse.y;
		switch (e.eventType) {
		case vr::VREvent_MouseMove: {
			const int key = KeyAt(x, y);
			const bool onClose = mode != Mode::Minimal && CloseRect().Contains(x, y);
			if (key != hovered || onClose != closeHovered) {
				hovered = key;
				closeHovered = onClose;
				dirty = true;
			}
			break;
		}
		case vr::VREvent_FocusLeave:
			if (hovered != -1 || closeHovered) {
				hovered = -1;
				closeHovered = false;
				dirty = true;
			}
			break;
		case vr::VREvent_MouseButtonDown: {
			if (mode != Mode::Minimal && CloseRect().Contains(x, y)) {
				Close();
				break;
			}
			const int key = KeyAt(x, y);
			if (key >= 0 && layout)
				Press(layout->GetKeymap()[key]);
			break;
		}
		default:
			break;
		}
	}
}

void VRKeyboard::Draw(Canvas& canvas) const
{
	canvas.Clear(kBackground);
	const int line = Canvas::LineHeight();
	if (layout) {
		for (size_t i = 0; i < layout->GetKeymap().size(); i++) {
			const KeyboardLayout::Key& key = layout->GetKeymap()[i];
			const Rect r = KeyRect(key);
			const bool lit = (key.ch == kShiftKey && caseMode == Case::Shift) || (key.ch == kCapsKey && caseMode == Case::Lock);
			canvas.FillRect(r.x, r.y, r.w, r.h, lit ? kKeyLit : ((int)i == hovered ? kKeyHovered : kKey), 6);
			const std::wstring& label = caseMode == Case::Lower ? key.label : key.labelShift;
			const int labelWidth = canvas.TextWidth(label);
			canvas.Text(r.x + (r.w - labelWidth) / 2, r.y + (r.h - line) / 2, label, lit ? kLabelLit : kLabel);
		}
	}
	if (mode == Mode::Minimal)
		return;

	const Rect box = TextRect();
	canvas.FillRect(box.x, box.y, box.w, box.h, kTextBox, 4);
	// A password as stars; a long text shows its end
	std::wstring shown = mode == Mode::Password ? std::wstring(chars.size(), L'*') : Wide(chars);
	const int room = box.w - 2 * kPadding;
	// Walked back from the end, adding each advance: the start of what fits
	{
		int used = 0;
		size_t start = shown.size();
		while (start > 0) {
			const int advance = canvas.TextWidth(std::wstring_view(shown).substr(start - 1, 1));
			if (used + advance > room)
				break;
			used += advance;
			start--;
		}
		shown.erase(0, start);
	}
	canvas.Text(box.x + kPadding, box.y + (box.h - line) / 2, shown, kText);

	const Rect close = CloseRect();
	canvas.FillRect(close.x, close.y, close.w, close.h, closeHovered ? kCloseHovered : kClose, 6);
	const int xWidth = canvas.TextWidth(L"x");
	canvas.Text(close.x + (close.w - xWidth) / 2, close.y + (close.h - line) / 2, L"x", kLabel);
}
