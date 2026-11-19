#pragma once

// What a binding file binds an action to, as IVRInput::GetActionBindingInfo tells a game: each input of each source
// (and each input of a chord) that writes the action.

#include "BindingFile.h"

#include <string>
#include <vector>

struct ActionBinding {
	std::string device; // /user/hand/left
	std::string input; // /input/trigger
	std::string mode; // button, dpad, trigger, joystick... ("chord" for a chord's input)
	std::string slot; // click, touch, pull, position, north...
	std::string sourceType; // what the user moves: button, trigger, joystick or trackpad
};

/** The bindings of an action (its manifest name, lower-cased) on one device (/user/hand/left; empty for every
 * device), in the file's order. A source of mode none binds nothing. */
std::vector<ActionBinding> BindingsForAction(const BindingFile& file, const std::string& action, const std::string& device = {});
