#include "logging.h"
#include "stdafx.h"

#include "KeyboardLayout.h"
#include "VRKeyboard.h"
#include "Misc/BuiltInResource.h"
#include "Misc/Config.h"
#include "Misc/Numbers.h"
#include "resources.h"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <memory>
#include <mutex>

static void wstrim(wstring& s)
{
	s.erase(s.begin(), find_if(s.begin(), s.end(), [](wchar_t ch) {
		return !std::iswspace(ch);
	}));

	s.erase(std::find_if(s.rbegin(), s.rend(), [](wchar_t ch) {
		return !std::iswspace(ch);
	}).base(),
	    s.end());
}

static wstring pullstring(wstring& in)
{
	size_t wordlen = 0;

	while (wordlen < in.length() && !std::iswspace(in[wordlen])) {
		wordlen++;
	}

	wstring word = in.substr(0, wordlen);

	// Chop the spaces off the remander
	size_t spacelen = wordlen;
	while (spacelen < in.length() && std::iswspace(in[spacelen])) {
		spacelen++;
	}

	in.erase(0, spacelen);

	return word;
}

static wchar_t escapeChar(const wstring& str)
{
	if (str[0] != '\\')
		return str[0];

	wchar_t esc = str[1];

	switch (esc) {
	case 't':
		return '\t';
	case '\\':
		return '\\';
	case 'n':
		return '\n';
	case 'b':
		return '\b'; // Backspace
	case 's':
		return ' '; // For whitespace-deliminated stuff
	case 'z':
		return '\x01'; // Shift
	case 'c':
		return '\x02'; // Caps lock
	case 'q':
		return '\x03'; // Done
	}

	string utf = VRKeyboard::WideToUtf8(str);

	{ OOVR_LOGF("Unknown escape sequence '%s'", utf.c_str()); return 0; } // the built-in layout is broken: no keyboard
}

KeyboardLayout::KeyboardLayout(std::vector<char> data)
{
	wstring contents = VRKeyboard::Utf8ToWide(string(data.data(), data.size()));

	Key* last = nullptr;

	auto addKey = [&](wchar_t ch, wchar_t shift, float x, float y) -> Key& {
		int id = (int)keys.size();

		keys.push_back(Key());
		Key& key = keys.back();
		last = &key;

		key = { 0 };

		key.id = id;

		key.ch = ch;
		key.shift = shift;

		key.x = x;
		key.y = y;

		key.label = ch;
		key.labelShift = shift;

		key.w = 1;
		key.h = 1;

		return key;
	};

	auto addNextKey = [&](wchar_t ch, wchar_t shift) {
		if (!last)
			{ OOVR_LOG("Cannot add bank or unpositioned key as first key!"); return; } // the built-in layout is broken: no keyboard

		addKey(ch, shift, last->x + last->w, last->y);
	};

	while (!contents.empty()) {
		size_t newlinePos = contents.find_first_of('\n');
		wstring line = contents.substr(0, newlinePos);
		contents.erase(0, newlinePos + 1);

		wstrim(line);
		if (line.empty())
			continue;

		// Comments
		if (line[0] == '#')
			continue;

		wstring op = pullstring(line);

		if (op == L"bank") {
			wstring chars = pullstring(line);
			wstring shift = pullstring(line);

			if (chars.length() != shift.length())
				{ OOVR_LOGF("Keyboard layout: bank lower/upper length mismatch: '%ls' vs '%ls'", chars.c_str(), shift.c_str()); return; } // the built-in layout is broken: no keyboard

			for (size_t i = 0; i < chars.length(); i++) {
				addNextKey(chars[i], shift[i]);
			}
			continue;
		}

		if (op == L"width") {
			width = std::stoi(pullstring(line));
			continue;
		}

		if (op[0] == '.') {
			wstring prop = op.substr(1);
			string utfProp = VRKeyboard::WideToUtf8(prop);

			if (!last) {
				{ OOVR_LOGF("Cannot set property '%s' without a prior key!", utfProp.c_str()); return; } // the built-in layout is broken: no keyboard
			}

			if (prop == L"spans_to_right") {
				last->spansToRight = pullstring(line) != L"false";
				continue;
			} else if (prop == L"label") {
				last->label = pullstring(line);
				last->labelShift = last->label;
				continue;
			}

			{ OOVR_LOGF("Unknown property '%s' for key '%s'", utfProp.c_str(), VRKeyboard::WideToUtf8(last->label).c_str()); return; } // the built-in layout is broken: no keyboard

			continue;
		}

		wchar_t ch = escapeChar(pullstring(line));
		wchar_t shift = escapeChar(pullstring(line));

		wstring wx, wy;

		wx = pullstring(line);
		wy = pullstring(line);

		if (wx.empty() != wy.empty()) {
			{ OOVR_LOGF("x/y empty mismatch for keyboard char '%c'", (char)ch); return; } // the built-in layout is broken: no keyboard
		}

		if (wx.empty()) {
			addNextKey(ch, shift);
		} else {
			Key& key = addKey(ch, shift, Numbers::ToFloat(wx, 0), Numbers::ToFloat(wy, 0));

			wstring ww = pullstring(line);
			wstring wh = pullstring(line);

			if (!ww.empty()) {
				if (wh.empty()) {
					{ OOVR_LOGF("word width/height mismatch for keyboard char '%c'", ch); return; } // the built-in layout is broken: no keyboard
				}

				key.w = Numbers::ToFloat(ww, key.w);
				key.h = Numbers::ToFloat(wh, key.h);
			}
		}
	}

	if (width == 0)
		{ OOVR_LOG("Missing keyboard layout width specifier"); return; } // the built-in layout is broken: no keyboard
	valid = !keys.empty();
}

KeyboardLayout::~KeyboardLayout()
{
}

float KeyboardLayout::GetHeight() const
{
	float height = 0;
	for (const Key& key : keys)
		height = std::max(height, key.y + key.h);
	return height;
}

const KeyboardLayout* KeyboardLayout::Get(Name name)
{
	static std::once_flag once[2];
	static std::unique_ptr<KeyboardLayout> layouts[2];
	const int i = name == Name::UK ? 0 : 1;
	std::call_once(once[i], [&] {
		std::vector<char> data = ReadBuiltInResource(name == Name::UK ? RES_O_KB_EN_GB : RES_O_KB_EN_US, RES_T_KBLAYOUT);
		if (data.empty()) {
			OOVR_LOGF("The %s keyboard layout isn't built in", name == Name::UK ? "UK" : "US");
			return;
		}
		auto layout = std::make_unique<KeyboardLayout>(data);
		if (layout->Valid())
			layouts[i] = std::move(layout);
	});
	return layouts[i].get();
}

KeyboardLayout::Name KeyboardLayout::ForLocale(const std::string& locale)
{
	std::string l = locale;
	for (char& c : l)
		c = c == '_' ? '-' : (char)std::tolower((unsigned char)c);
	return l.rfind("en-gb", 0) == 0 ? Name::UK : Name::US;
}

const KeyboardLayout* KeyboardLayout::Current()
{
	const std::string chosen = oovr_global_configuration.KeyboardLayout();
	if (chosen == "uk")
		return Get(Name::UK);
	if (chosen == "us")
		return Get(Name::US);
	std::string locale;
#ifdef _WIN32
	wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
	if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) > 0)
		locale = VRKeyboard::WideToUtf8(name);
#else
	for (const char* var : { "LC_ALL", "LC_MESSAGES", "LANG" }) {
		const char* value = getenv(var);
		if (value && *value) {
			locale = value;
			break;
		}
	}
#endif
	return Get(ForLocale(locale));
}
