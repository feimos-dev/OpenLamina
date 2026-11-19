#pragma once

// SteamVR binding files as a model: controller_type, then per action set its sources (a mode on an input source,
// with parameters and named inputs bound to actions), poses, haptics, skeleton and chords; and the simulated actions.
// Paths and outputs are lower-cased, as SteamVR matches them. Reading never throws: anything unknown is skipped with a
// warning.

#include "json/json.h"

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct SourceBinding {
	std::string path, mode;
	std::map<std::string, std::string> parameters; // values as text, as the files have them
	std::map<std::string, std::string> inputs; // input name (click, pull, position...) → output action path
};
struct PoseBinding {
	std::string output, path;
	std::map<std::string, std::string> parameters;
};
struct HapticBinding {
	std::string output, path;
	std::map<std::string, std::string> parameters;
};
struct SkeletonBinding {
	std::string output, path;
};
struct ChordBinding {
	std::string output;
	std::vector<std::pair<std::string, std::string>> inputs; // (source path, input name)
};
struct SimulatedAction {
	std::string name, type;
	Json::Value raw;
};
struct ActionSetBindings {
	std::vector<SourceBinding> sources;
	std::vector<PoseBinding> poses;
	std::vector<HapticBinding> haptics;
	std::vector<SkeletonBinding> skeleton;
	std::vector<ChordBinding> chords;
};
struct BindingFile {
	std::string controllerType;
	std::map<std::string, ActionSetBindings> sets; // by action set path, lower-cased
	std::vector<SimulatedAction> simulatedActions;
	std::vector<std::string> warnings;
};

BindingFile ParseBindingFile(std::string_view text);
BindingFile LoadBindingFile(const std::filesystem::path& path);

/** Whether a source of the file is on one of rawPaths (the raw inputs of a controller, such as
 * /user/hand/right/input/trigger/value, for the source /user/hand/right/input/trigger) and binds an input to an output
 * that wanted accepts, given its action set's path */
bool BindsRawInput(const BindingFile& file, const std::vector<std::string>& rawPaths,
    const std::function<bool(const std::string& setPath, const std::string& output)>& wanted);

/** The names an action manifest's "localization" tables and "localization_files" give, for the table of the language
 * closest to language (a tag such as "en_US"): lower-cased action and action set path ("/actions/main/in/fire") to
 * its text. Files are read from manifestDir. A table or file that can't be used is skipped, never an error. */
std::map<std::string, std::string> ReadManifestLocalization(const Json::Value& manifest, const std::filesystem::path& manifestDir,
    std::string_view language);
