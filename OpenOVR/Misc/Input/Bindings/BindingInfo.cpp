#include "stdafx.h"

#include "BindingInfo.h"

namespace {
// /user/hand/left/input/trigger -> (/user/hand/left, /input/trigger); false for a path without /input/
bool SplitSource(const std::string& path, std::string& device, std::string& input)
{
	size_t at = path.find("/input/");
	if (at == std::string::npos || at == 0)
		return false;
	device = path.substr(0, at);
	input = path.substr(at);
	return true;
}

// SteamVR takes this from the controller's driver; the inputs' names say as much for the controllers there are. A
// grip is a trigger where it's squeezed (Index, Touch) and a button where it clicks (Vive, WMR), which the file's
// controller type tells.
std::string SourceType(const std::string& input, const std::string& controllerType)
{
	const std::string name = input.substr(input.rfind('/') + 1);
	if (name == "trigger")
		return "trigger";
	if (name == "grip")
		return controllerType == "knuckles" || controllerType == "oculus_touch" ? "trigger" : "button";
	if (name == "joystick" || name == "thumbstick")
		return "joystick";
	if (name == "trackpad")
		return "trackpad";
	return "button";
}
} // namespace

std::vector<ActionBinding> BindingsForAction(const BindingFile& file, const std::string& action, const std::string& device)
{
	std::vector<ActionBinding> found;
	auto add = [&](const std::string& path, const std::string& mode, const std::string& slot) {
		ActionBinding b;
		if (!SplitSource(path, b.device, b.input) || (!device.empty() && b.device != device))
			return;
		b.mode = mode;
		b.slot = slot;
		b.sourceType = SourceType(b.input, file.controllerType);
		found.push_back(std::move(b));
	};
	for (const auto& [setName, set] : file.sets) {
		for (const SourceBinding& source : set.sources) {
			if (source.mode == "none")
				continue;
			for (const auto& [slot, output] : source.inputs)
				if (output == action)
					add(source.path, source.mode, slot);
		}
		for (const ChordBinding& chord : set.chords) {
			if (chord.output != action)
				continue;
			for (const auto& [path, slot] : chord.inputs)
				add(path, "chord", slot);
		}
	}
	return found;
}
