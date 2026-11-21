#pragma once

// What a binding file binds an action to, as IVRInput::GetActionBindingInfo tells a game: each input of each source
// (and each input of a chord) that writes the action.

#include "BindingFile.h"

#include <map>
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
 * device). A source of mode none binds nothing, nor a force sensor's click. A pose and a skeleton are told by their
 * device and the path read (/pose/raw, /input/skeleton/left) with no mode; a vibration with every field empty. */
std::vector<ActionBinding> BindingsForAction(const BindingFile& file, const std::string& action, const std::string& device = {});

/** The render model component an origin's input path names: the input's name, so /user/hand/left/input/trigger/click
 * is "trigger". Empty for a path that names no input (a hand, a pose). */
std::string OriginComponentName(const std::string& originPath);

/** The origin of an input path: the device and the input, without the part of the input that is read
 * (/user/hand/left/input/trigger/click is /user/hand/left/input/trigger). A path with no input is returned as it is. */
std::string OriginFromInputPath(const std::string& path);

/** The hands (/user/hand/left) a file binds a pose or a skeleton action on (the action's manifest name, lower-cased), in
 * the order the file lists them, each once. */
std::vector<std::string> PoseDevicesForAction(const BindingFile& file, const std::string& action);

/** What SteamVR calls an origin, in the pieces a game asks for: "Left Hand", the controller ("Oculus Touch") and the
 * input ("Trigger"), joined by spaces. hand is 0 for the left hand, 1 for the right, anything else for none; the
 * controller type is the OpenVR name of its profile, empty if it has none; the input is the origin's path. */
std::string OriginLocalizedName(int hand, const std::string& controllerType, const std::string& originPath,
    bool withHand, bool withController, bool withInput);
