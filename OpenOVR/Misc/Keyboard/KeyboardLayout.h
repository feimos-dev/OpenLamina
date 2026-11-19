#pragma once
#include <map>
#include <string>
#include <vector>

class KeyboardLayout {
public:
	struct Key {
		int id;

		wchar_t ch, shift;
		float x, y;

		std::wstring label, labelShift;
		float w, h;

		// Is the key touching the right-most side of the keyboard?
		bool spansToRight;
	};

	using keymap_t = std::vector<Key>;

	KeyboardLayout(std::vector<char>);
	~KeyboardLayout();

	const keymap_t& GetKeymap() const { return keys; }
	int GetWidth() const { return width; }
	// Every line was understood
	bool Valid() const { return valid; }
	// The rows, the bottom of the lowest key
	float GetHeight() const;

	// The built-in layouts
	enum class Name { UK, US };
	// Loaded once; null if it can't be read
	static const KeyboardLayout* Get(Name name);
	// keyboardLayout's (us or uk); without it, the UK one where the user's locale is en-GB and the US one elsewhere
	static const KeyboardLayout* Current();
	// en-GB (or en_GB.UTF-8 and the like) is UK; anything else US
	static Name ForLocale(const std::string& locale);

private:
	keymap_t keys;
	int width = 0;
	bool valid = false;
};
