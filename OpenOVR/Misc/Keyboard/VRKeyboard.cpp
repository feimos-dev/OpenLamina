#include "stdafx.h"

#include "VRKeyboard.h"

#include "Misc/Canvas.h"
#include "Misc/LaserMouse.h"

#include <algorithm>
#include <cstring>

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
constexpr uint32_t kHint = 0x808080ff;
constexpr uint32_t kSelection = 0x9ec5f8ff;
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

std::u32string VRKeyboard::FromUtf8Text(const std::string& utf8)
{
	return FromUtf8(utf8);
}

std::string VRKeyboard::TextWithoutKeyboard(const std::string& existing)
{
	return existing.empty() ? "Adventurer" : existing;
}

void VRKeyboard::SetText()
{
	text.clear();
	for (char32_t k : chars)
		text += Utf8(k);
}

std::string VRKeyboard::WideToUtf8(const std::wstring& s)
{
	std::string out;
	for (size_t i = 0; i < s.size(); i++) {
		char32_t c = (char32_t)s[i];
		if (sizeof(wchar_t) == 2 && c >= 0xd800 && c < 0xdc00) {
			// A surrogate pair; a lone one is left out
			if (i + 1 >= s.size() || (char32_t)s[i + 1] < 0xdc00 || (char32_t)s[i + 1] > 0xdfff)
				continue;
			c = 0x10000 + ((c - 0xd800) << 10) + ((char32_t)s[++i] - 0xdc00);
		} else if ((c >= 0xd800 && c <= 0xdfff) || c > 0x10ffff) {
			continue;
		}
		out += Utf8(c);
	}
	return out;
}

std::wstring VRKeyboard::Utf8ToWide(const std::string& s)
{
	std::wstring out;
	for (char32_t c : FromUtf8(s)) {
		if (sizeof(wchar_t) == 2 && c >= 0x10000) {
			c -= 0x10000;
			out += (wchar_t)(0xd800 + (c >> 10));
			out += (wchar_t)(0xdc00 + (c & 0x3ff));
		} else {
			out += (wchar_t)c;
		}
	}
	return out;
}

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
		SetText();
		caret = anchor = chars.size();
	}
	Send(vr::VREvent_KeyboardOpened_Global);
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

void VRKeyboard::Finish(bool withDone)
{
	if (withDone) {
		Send(vr::VREvent_KeyboardDone);
		done = true;
		// A chat line stays open for the next one
		if (mode == Mode::Submit) {
			dirty = true;
			return;
		}
	}
	closed = true;
	Send(vr::VREvent_KeyboardClosed);
	Send(vr::VREvent_KeyboardClosed_Global);
	dirty = true;
}

void VRKeyboard::Type(char32_t c)
{
	if (closed)
		return;
	if (c == kDone || (c == U'\n' && !multiline)) {
		Finish(true);
		return;
	}
	if (c == U'\t' && !multiline)
		return;
	if (c == U'\b') {
		if (DeleteSelection())
			return;
		if (mode != Mode::Minimal && caret > 0) {
			chars.erase(caret - 1, 1);
			caret--;
			anchor = caret;
			SetText();
		}
		Send(vr::VREvent_KeyboardCharInput, "\b");
		dirty = true;
		return;
	}
	if (c < 0x20 && c != U'\n' && c != U'\t')
		return; // the layout's other controls type nothing
	if (mode != Mode::Minimal) {
		// What's typed replaces the selection
		DeleteSelection();
		if (maxChars && chars.size() >= maxChars)
			return;
		chars.insert(caret, 1, c);
		caret++;
		anchor = caret;
		SetText();
	}
	Send(vr::VREvent_KeyboardCharInput, Utf8(c));
	dirty = true;
}

void VRKeyboard::Close()
{
	selecting = false;
	if (closed)
		return;
	Finish(false);
}

void VRKeyboard::PressArrow(int arrow)
{
	if (mode == Mode::Minimal) {
		// The ANSI escape sequence of the key
		static const char* const sequences[4] = { "\x1b[D", "\x1b[C", "\x1b[A", "\x1b[B" };
		Send(vr::VREvent_KeyboardCharInput, sequences[arrow]);
		return;
	}
	// Up and down move by lines in a multiline text; in a single line they go to its start and end
	auto lineStart = [&](size_t at) { return LineStart(at); };
	auto lineEnd = [&](size_t at) { return LineEnd(at); };
	size_t to = caret;
	const auto [selFrom, selTo] = Selection();
	switch (arrow) {
	case Left:
		// A selection collapses to its start
		to = selFrom != selTo ? selFrom : caret > 0 ? caret - 1 : 0;
		break;
	case Right:
		to = selFrom != selTo ? selTo : caret < chars.size() ? caret + 1 : caret;
		break;
	case Up:
		if (!multiline) {
			to = 0;
		} else {
			const size_t start = lineStart(caret);
			if (start > 0) {
				const size_t column = caret - start;
				const size_t previousStart = lineStart(start - 1);
				to = previousStart + std::min(column, start - 1 - previousStart);
			}
		}
		break;
	default:
		if (!multiline) {
			to = chars.size();
		} else {
			const size_t end = lineEnd(caret);
			if (end < chars.size()) {
				const size_t column = caret - lineStart(caret);
				const size_t nextStart = end + 1;
				to = nextStart + std::min(column, lineEnd(nextStart) - nextStart);
			}
		}
		break;
	}
	if (to != caret || anchor != caret) {
		caret = anchor = to;
		dirty = true;
	}
}

size_t VRKeyboard::LineStart(size_t at) const
{
	while (at > 0 && chars[at - 1] != U'\n')
		at--;
	return at;
}

size_t VRKeyboard::LineEnd(size_t at) const
{
	while (at < chars.size() && chars[at] != U'\n')
		at++;
	return at;
}

void VRKeyboard::Select(size_t from, size_t to)
{
	from = std::min(from, chars.size());
	to = std::min(to, chars.size());
	if (anchor != from || caret != to) {
		anchor = from;
		caret = to;
		dirty = true;
	}
}

bool VRKeyboard::DeleteSelection()
{
	const auto [from, to] = Selection();
	if (mode == Mode::Minimal || from == to)
		return false;
	chars.erase(from, to - from);
	caret = anchor = from;
	SetText();
	for (size_t i = from; i < to; i++)
		Send(vr::VREvent_KeyboardCharInput, "\b");
	dirty = true;
	return true;
}

std::wstring VRKeyboard::Shown(size_t& from) const
{
	// A multiline text shows the caret's line, scrolled to as the caret moves between lines
	from = multiline ? LineStart(caret) : 0;
	const size_t to = multiline ? LineEnd(caret) : chars.size();
	const std::u32string part = chars.substr(from, to - from);
	return mode == Mode::Password ? std::wstring(part.size(), L'*') : Wide(part);
}

size_t VRKeyboard::CharAt(int x) const
{
	size_t from = 0;
	const std::wstring shown = Shown(from);
	const Rect box = TextRect();
	Canvas measure(1, 1);
	size_t at = std::clamp(viewStart, from, from + shown.size()) - from;
	int left = box.x + kPadding;
	while (at < shown.size()) {
		const int advance = measure.TextWidth(std::wstring_view(shown).substr(at, 1));
		// Past the character's middle: after it
		if (x < left + advance / 2)
			break;
		left += advance;
		at++;
	}
	return from + at;
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
		// Hide closes without saying it's done
		if (flags & kFlagHideDoneKey)
			Close();
		else
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
	// Below the text box and the arrow keys' row, which a minimal keyboard has only with arrow keys
	return mode == Mode::Minimal && !ShowsArrows() ? kPadding : kPadding + Pitch();
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
	// Between the left edge and the arrow keys, which are beside the close button
	const int arrows = ShowsArrows() ? 4 * (KeySize() + kPadding) : 0;
	return Rect{ kPadding, kPadding, kWidth - 3 * kPadding - KeySize() - arrows, KeySize() };
}

VRKeyboard::Rect VRKeyboard::ArrowRect(int arrow) const
{
	// The top row, left of the close button (at the right edge on a minimal keyboard, which has none)
	const int right = mode == Mode::Minimal ? kWidth - kPadding : CloseRect().x - kPadding;
	return Rect{ right - (4 - arrow) * (KeySize() + kPadding) + kPadding, kPadding, KeySize(), KeySize() };
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
			int arrow = -1;
			for (int a = 0; a < 4 && ShowsArrows(); a++)
				if (ArrowRect(a).Contains(x, y))
					arrow = a;
			if (selecting && mode != Mode::Minimal) {
				// Dragging across the text selects; the view follows the caret
				const size_t to = CharAt(x);
				if (to != caret) {
					caret = to;
					dirty = true;
				}
			}
			if (key != hovered || onClose != closeHovered || arrow != hoveredArrow) {
				hovered = key;
				closeHovered = onClose;
				hoveredArrow = arrow;
				dirty = true;
			}
			break;
		}
		case vr::VREvent_FocusLeave:
			selecting = false;
			if (hovered != -1 || closeHovered || hoveredArrow != -1) {
				hovered = -1;
				closeHovered = false;
				hoveredArrow = -1;
				dirty = true;
			}
			break;
		case vr::VREvent_MouseButtonDown: {
			if (mode != Mode::Minimal && CloseRect().Contains(x, y)) {
				Close();
				break;
			}
			if (mode != Mode::Minimal && TextRect().Contains(x, y)) {
				// The caret goes where the text box is clicked, and a drag from there selects
				const size_t at = CharAt(x);
				Select(at, at);
				selecting = true;
				break;
			}
			bool arrowPressed = false;
			for (int a = 0; a < 4 && ShowsArrows(); a++) {
				if (ArrowRect(a).Contains(x, y)) {
					PressArrow(a);
					arrowPressed = true;
				}
			}
			const int key = KeyAt(x, y);
			if (!arrowPressed && key >= 0 && layout)
				Press(layout->GetKeymap()[key]);
			break;
		}
		case vr::VREvent_MouseButtonUp:
			selecting = false;
			break;
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
			std::wstring label = caseMode == Case::Lower ? key.label : key.labelShift;
			if (key.ch == kDoneKey && (flags & kFlagHideDoneKey))
				label = L"Hide";
			const int labelWidth = canvas.TextWidth(label);
			canvas.Text(r.x + (r.w - labelWidth) / 2, r.y + (r.h - line) / 2, label, lit ? kLabelLit : kLabel);
		}
	}
	if (ShowsArrows()) {
		static const wchar_t* const arrows[4] = { L"<", L">", L"^", L"v" };
		for (int a = 0; a < 4; a++) {
			const Rect r = ArrowRect(a);
			canvas.FillRect(r.x, r.y, r.w, r.h, a == hoveredArrow ? kKeyHovered : kKey, 6);
			canvas.Text(r.x + (r.w - canvas.TextWidth(arrows[a])) / 2, r.y + (r.h - line) / 2, arrows[a], kLabel);
		}
	}
	if (mode == Mode::Minimal)
		return;

	const Rect box = TextRect();
	canvas.FillRect(box.x, box.y, box.w, box.h, kTextBox, 4);
	// A password as stars; a long text shows a part, which scrolls only as far as keeps the caret in view
	size_t from = 0;
	const std::wstring shown = Shown(from);
	const size_t at = caret - from; // the caret in shown
	const int room = box.w - 2 * kPadding;
	auto width = [&](size_t a, size_t b) { return canvas.TextWidth(std::wstring_view(shown).substr(a, b - a)); };
	size_t start = std::clamp(viewStart, from, from + shown.size()) - from;
	if (start > at)
		start = at;
	while (start < at && width(start, at) > room)
		start++;
	// Room left after the end: the view goes back to fill it
	while (start > 0 && width(start - 1, shown.size()) <= room)
		start--;
	viewStart = from + start;
	size_t end = start;
	while (end < shown.size() && width(start, end + 1) <= room)
		end++;
	const std::wstring visible = shown.substr(start, end - start);
	const int textY = box.y + (box.h - line) / 2;
	const int textX = box.x + kPadding;
	// The selection, behind the text, where it's in view
	const auto [selFrom, selTo] = Selection();
	if (selFrom != selTo) {
		const size_t a = std::clamp(selFrom, from + start, from + end) - from;
		const size_t b = std::clamp(selTo, from + start, from + end) - from;
		if (a < b)
			canvas.FillRect(textX + width(start, a), box.y + 4, width(a, b), box.h - 8, kSelection);
	}
	if (chars.empty() && !description.empty())
		canvas.Text(textX, textY, Wide(description), kHint);
	canvas.Text(textX, textY, visible, kText);
	if (!closed) {
		const int caretX = textX + width(start, at);
		canvas.FillRect(caretX, box.y + 4, 2, box.h - 8, kText);
	}

	const Rect close = CloseRect();
	canvas.FillRect(close.x, close.y, close.w, close.h, closeHovered ? kCloseHovered : kClose, 6);
	const int xWidth = canvas.TextWidth(L"x");
	canvas.Text(close.x + (close.w - xWidth) / 2, close.y + (close.h - line) / 2, L"x", kLabel);
}
