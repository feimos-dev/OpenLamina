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
// grip is a trigger where it's squeezed and a button where it clicks, which the file's controller type tells.
std::string SourceType(const std::string& input, const std::string& controllerType)
{
	if (input.starts_with("/input/finger/"))
		return "trigger";
	const std::string name = input.substr(input.rfind('/') + 1);
	if (name == "trigger")
		return "trigger";
	if (name == "pinch")
		return "pinch";
	if (name == "grip") {
		const bool clicks = controllerType == "vive_controller" || controllerType == "holographic_controller" || controllerType == "vive_tracker";
		return clicks ? "button" : "trigger";
	}
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
	// A pose and a skeleton are told by where they're read (/pose/raw, /input/skeleton/left), with no mode
	auto addPose = [&](const std::string& path) {
		ActionBinding b;
		size_t at = path.find("/pose");
		if (at == std::string::npos)
			at = path.find("/input/");
		if (at == std::string::npos || at == 0)
			return;
		b.device = path.substr(0, at);
		b.input = path.substr(at);
		if (device.empty() || b.device == device)
			found.push_back(std::move(b));
	};
	for (const auto& [setName, set] : file.sets) {
		for (const SourceBinding& source : set.sources) {
			if (source.mode == "none")
				continue;
			for (const auto& [slot, output] : source.inputs) {
				// A force sensor is read by its force; one bound by its click binds nothing
				if (output == action && !(source.mode == "force_sensor" && slot == "click"))
					add(source.path, source.mode, slot);
			}
		}
		for (const PoseBinding& pose : set.poses)
			if (pose.output == action)
				addPose(pose.path);
		for (const SkeletonBinding& skeleton : set.skeleton)
			if (skeleton.output == action)
				addPose(skeleton.path);
		// A vibration is told with no device or input path
		for (const HapticBinding& haptic : set.haptics) {
			if (haptic.output != action)
				continue;
			const size_t at = haptic.path.find("/output/");
			if (!device.empty() && (at == std::string::npos || haptic.path.substr(0, at) != device))
				continue;
			found.emplace_back();
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

namespace {
// The part of an origin path after /input/, up to the next /
std::string InputName(const std::string& path)
{
	static const std::string marker = "/input/";
	size_t at = path.find(marker);
	if (at == std::string::npos)
		return {};
	size_t start = at + marker.size();
	return path.substr(start, path.find('/', start) - start);
}

// "application_menu" is "Application Menu"; a lone letter is a button
std::string TitleCase(const std::string& name)
{
	std::string out;
	bool startOfWord = true;
	for (char c : name) {
		if (c == '_' || c == '/') {
			out += ' ';
			startOfWord = true;
			continue;
		}
		out += startOfWord ? (char)toupper((unsigned char)c) : c;
		startOfWord = false;
	}
	if (name.size() == 1)
		out += " Button";
	return out;
}

std::string ControllerName(const std::string& type)
{
	static const std::map<std::string, std::string> names = {
		{ "oculus_touch", "Oculus Touch" },
		{ "vive_controller", "Vive Controller" },
		{ "knuckles", "Index Controller" },
		{ "holographic_controller", "Windows Mixed Reality Controller" },
		{ "hpmotioncontroller", "HP Reverb G2 Controller" },
		{ "vive_cosmos_controller", "Vive Cosmos Controller" },
		{ "vive_focus3_controller", "Vive Focus 3 Controller" },
		{ "pico_controller", "Pico Controller" },
		{ "vive_tracker", "Vive Tracker" },
	};
	auto found = names.find(type);
	return found != names.end() ? found->second : "OpenXR Controller";
}
} // namespace

std::string OriginComponentName(const std::string& originPath)
{
	return InputName(originPath);
}

std::string OriginFromInputPath(const std::string& path)
{
	static const std::string marker = "/input/";
	size_t at = path.find(marker);
	if (at == std::string::npos)
		return path;
	size_t end = path.find('/', at + marker.size());
	return end == std::string::npos ? path : path.substr(0, end);
}

std::vector<std::string> PoseDevicesForAction(const BindingFile& file, const std::string& action)
{
	std::vector<std::string> devices;
	auto add = [&](const std::string& path) {
		size_t end = path.find("/pose");
		if (end == std::string::npos)
			end = path.find("/input/");
		std::string device = path.substr(0, end);
		if (!device.empty() && std::find(devices.begin(), devices.end(), device) == devices.end())
			devices.push_back(std::move(device));
	};
	for (const auto& [setName, set] : file.sets) {
		for (const PoseBinding& pose : set.poses)
			if (pose.output == action)
				add(pose.path);
		for (const SkeletonBinding& skeleton : set.skeleton)
			if (skeleton.output == action)
				add(skeleton.path);
	}
	return devices;
}

std::string OriginLocalizedName(int hand, const std::string& controllerType, const std::string& originPath,
    bool withHand, bool withController, bool withInput)
{
	std::string name;
	auto append = [&](const std::string& part) {
		if (part.empty())
			return;
		if (!name.empty())
			name += ' ';
		name += part;
	};
	if (withHand)
		append(hand == 0 ? "Left Hand" : hand == 1 ? "Right Hand" : "");
	if (withController)
		append(ControllerName(controllerType));
	if (withInput)
		append(TitleCase(InputName(originPath)));
	return name;
}
