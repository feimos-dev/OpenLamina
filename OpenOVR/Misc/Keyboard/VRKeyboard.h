#pragma once
// The keyboard IVROverlay::ShowKeyboard shows: drawn on a Canvas from its layout and the built-in font, typed on with
// the controllers' lasers (LaserMouse), and shown as a quad from a swapchain filled from memory, on any graphics API.

#include <openxr/openxr.h>

#include <cstdint>
#include <string>
#include <vector>

#include "KeyboardLayout.h"
#include "generated/interfaces/vrtypes.h"

class Canvas;
class LaserMouse;
struct PointerEvent;

class VRKeyboard {
public:
	enum class Mode {
		Normal,
		Password, // the text is drawn as stars
		Submit, // Done doesn't close it (a chat line)
		Minimal, // each key is sent, with no text box
	};

	// EKeyboardFlags
	static constexpr uint32_t kFlagMinimal = 1;
	static constexpr uint32_t kFlagModal = 2; // nothing but the keyboard can be pointed at, and a click off it closes it
	static constexpr uint32_t kFlagShowArrowKeys = 4; // arrow keys in minimal mode (a buffered keyboard always has them)
	static constexpr uint32_t kFlagHideDoneKey = 8; // a Hide key that only closes, in place of Done

	// flags: EKeyboardFlags (both overloads of ShowKeyboard give them); maxChars 0 for no limit. The layout is
	// keyboardLayout's (KeyboardLayout::Current).
	VRKeyboard(Mode mode, bool multiline, uint32_t flags, std::string initialText, uint32_t maxChars, uint64_t userValue);
	// With a given layout (for tests)
	VRKeyboard(Mode mode, bool multiline, uint32_t flags, std::string initialText, uint32_t maxChars, uint64_t userValue, const KeyboardLayout* layout);

	// Key presses from the lasers: the events of LaserMouse's last update that are for this keyboard (Handle())
	void OnPointer(const LaserMouse& mouse, const std::vector<PointerEvent>& events);
	// A character typed, as its key would type it: '\b' deletes, '\n' is Done on a single line, kDone is Done
	void Type(char32_t c);
	static constexpr char32_t kDone = 3;
	// An arrow key: moves the caret; a minimal keyboard sends its ANSI escape sequence
	enum Arrow { Left, Right, Up, Down };
	void PressArrow(int arrow);
	size_t Caret() const { return caret; }
	// Closed without Done (its close button, or a click off a modal keyboard): KeyboardClosed
	void Close();
	// What it asks for, shown in the empty text box
	void SetDescription(const std::string& utf8) { description = FromUtf8Text(utf8); }
	// The text GetKeyboardText answers where there is no keyboard to type on: what was there already, or a name
	// (Skyrim's character creation waits for one) where nothing was
	static std::string TextWithoutKeyboard(const std::string& existing);
	uint64_t UserValue() const { return userValue; }
	bool Modal() const { return (flags & kFlagModal) != 0; }

	// UTF-8
	const std::string& Text() const { return text; }
	bool Done() const { return done; }
	bool Closed() const { return closed; }

	// The events since the last call: KeyboardOpened_Global, KeyboardCharInput per key (its UTF-8 in cNewInput),
	// KeyboardDone, KeyboardClosed and KeyboardClosed_Global. The caller fills in overlayHandle and sends them on.
	std::vector<vr::VREvent_t> TakeEvents();

	// Drawn as it looks now; the canvas is CanvasWidth() x CanvasHeight()
	void Draw(Canvas& canvas) const;
	int CanvasWidth() const { return kWidth; }
	int CanvasHeight() const;
	// Whether it looks different since the last Draw
	bool Dirty() const { return dirty; }
	void MarkDrawn() { dirty = false; }

	// Where it's drawn, its centre in the tracking space (SetKeyboardTransformAbsolute, SetKeyboardPositionForOverlay)
	void SetPlacement(const XrPosef& pose) { pose_ = pose; }
	const XrPosef& Placement() const { return pose_; }
	// Its size in metres: a millimetre per pixel
	float WidthMeters() const { return kWidth * kMetresPerPixel; }
	float HeightMeters() const { return CanvasHeight() * kMetresPerPixel; }

	// The pointer target it is (an address no overlay's handle can have)
	vr::VROverlayHandle_t Handle() const { return (vr::VROverlayHandle_t)(uintptr_t)this; }

	// UTF-8 to and from wchar_t strings (UTF-16 where wchar_t is 16 bits wide); what can't be converted is left out
	static std::string WideToUtf8(const std::wstring& s);
	static std::wstring Utf8ToWide(const std::string& s);

private:
	enum class Case { Lower, Shift, Lock };

	static constexpr int kWidth = 1000;
	static constexpr float kMetresPerPixel = 0.001f;
	static constexpr int kPadding = 8;

	struct Rect {
		int x, y, w, h;
		bool Contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
	};
	int KeySize() const;
	int Pitch() const { return KeySize() + kPadding; }
	int KeysTop() const;
	bool ShowsArrows() const { return mode != Mode::Minimal || (flags & kFlagShowArrowKeys); }
	Rect ArrowRect(int arrow) const;
	void Finish(bool withDone);
	static std::u32string FromUtf8Text(const std::string& utf8);
	void SetText();
	Rect KeyRect(const KeyboardLayout::Key& key) const;
	Rect TextRect() const;
	Rect CloseRect() const;
	int KeyAt(int x, int y) const;
	void Press(const KeyboardLayout::Key& key);
	void Send(vr::EVREventType type, const std::string& utf8 = "");

	const Mode mode;
	const bool multiline;
	const uint32_t flags;
	const uint32_t maxChars;
	const uint64_t userValue;
	const KeyboardLayout* layout;

	std::u32string chars;
	std::u32string description;
	size_t caret = 0; // where typing goes: an index into chars
	std::string text; // chars as UTF-8
	Case caseMode = Case::Lower;
	bool done = false;
	bool closed = false;
	bool dirty = true;
	int hovered = -1; // the key the laser is on
	bool closeHovered = false;
	int hoveredArrow = -1;
	std::vector<vr::VREvent_t> events;
	XrPosef pose_{ { 0, 0, 0, 1 }, { 0, 0, -1 } };
};
