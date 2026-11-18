#pragma once
// The keyboard IVROverlay::ShowKeyboard shows: drawn on a Canvas from its layout and the built-in font, typed on with
// the controllers' lasers (LaserMouse), and shown as a quad from a swapchain filled from memory, on any graphics API.

#include <openxr/openxr.h>

#include <codecvt>
#include <cstdint>
#include <locale>
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
	static constexpr uint32_t kFlagModal = 2; // nothing but the keyboard can be pointed at

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
	// Closed without Done (its close button): KeyboardClosed
	void Close();

	// UTF-8
	const std::string& Text() const { return text; }
	bool Done() const { return done; }
	bool Closed() const { return closed; }

	// The events since the last call: KeyboardCharInput per key (its UTF-8 in cNewInput), KeyboardDone,
	// KeyboardClosed. The caller fills in overlayHandle and sends them on.
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

	static std::wstring_convert<std::codecvt_utf8<wchar_t>> CHAR_CONV;

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
	std::string text; // chars as UTF-8
	Case caseMode = Case::Lower;
	bool done = false;
	bool closed = false;
	bool dirty = true;
	int hovered = -1; // the key the laser is on
	bool closeHovered = false;
	std::vector<vr::VREvent_t> events;
	XrPosef pose_{ { 0, 0, 0, 1 }, { 0, 0, -1 } };
};
