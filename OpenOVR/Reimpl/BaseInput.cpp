#include "generated/interfaces/vrtypes.h"
#include "logging.h"
#include "openxr/openxr.h"
#include "json/json.h"
#define BASE_IMPL
#include "BaseInput.h"
#include "Misc/AppManifest.h"
#include "Misc/Input/Bindings/BindingFile.h"
#include "Misc/Input/Bindings/BindingSearch.h"
#include "Misc/Input/Bindings/Conversion.h"
#include "Misc/Input/Bindings/LegacyBindings.h"
#include "Misc/ProgramEvents.h"
#include "Misc/Config.h"
#include <string>

#include <convert.h>

#include "Drivers/Backend.h"
#include "generated/static_bases.gen.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <codecvt>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <filesystem>
#include <glm/gtc/matrix_inverse.hpp>
#include <locale>
#include <map>
#include <mutex>
#include <unordered_set>
#include <math.h>
#include <numbers>
#include <optional>
#include <set>
#include <utility>

#include "Misc/Fade.h"
#include "Misc/JsonFile.h"
#include "Misc/xrmoreutils.h"

// Use RenderModels for the pose offsets, which are the same as component positions
#include "BaseRenderModels.h"

using namespace vr;

#include "../DrvOpenXR/XrBackend.h"

// On Android, the application must supply a function to load the contents of a file

/**
 * Macro for creating an Action object from a handle and verifying isn't invalid.
 * If it is invalid, it will cause the surrounding function to return VRInputError_InvalidHandle.
 * action_var_name is the name of the resulting Action object if the handle isn't invalid.
 */
#define GET_ACTION_FROM_HANDLE(action_var_name, handle)              \
	Action* action_var_name = cast_AH(handle);                       \
	do {                                                             \
		if (!action_var_name) {                                      \
			OOVR_LOG_ONCE("WARNING: Invalid action handle passed!"); \
			return VRInputError_InvalidHandle;                       \
		}                                                            \
	} while (0)

/**
 * Same as GET_ACTION_FROM_HANDLE but for ActionSets.
 */
#define GET_ACTION_SET_FROM_HANDLE(action_var_name, handle)          \
	ActionSet* action_var_name = cast_ASH(handle);                   \
	do {                                                             \
		if (!action_var_name) {                                      \
			OOVR_LOG_ONCE("WARNING: Invalid action handle passed!"); \
			return VRInputError_InvalidHandle;                       \
		}                                                            \
	} while (0)

// This is a duplicate from BaseClientCore.cpp
// Manifests and binding files as SteamVR reads them (BOM, comments, trailing commas: Misc/JsonFile.h); false with
// the reason in error if the file can't be read or isn't JSON
// path is UTF-8, as OpenVR passes it; one that isn't (an older game's ANSI path) is tried as the system code page on
// Windows, and is an error otherwise, never an exception
static bool ReadJson(const std::string& path, Json::Value& result, std::string& error)
{
#ifndef ANDROID
	try {
		std::u8string utf8(path.begin(), path.end());
		return ReadJsonFileLenient(std::filesystem::path(utf8), result, error);
	} catch (const std::exception& e) {
#ifdef _WIN32
		try {
			return ReadJsonFileLenient(std::filesystem::path(path), result, error);
		} catch (const std::exception&) {
		}
#endif
		error = std::string("can't use the path ") + path + ": " + e.what();
		result = Json::Value();
		return false;
	}
#else
	std::string contents = OpenComposite_Android_Load_Input_File(path.c_str());
	return ParseJsonLenient(contents, result, error);
#endif
}

// A binding with no action or no path (OpenXR couldn't make one, logged) is left out: one bad entry would make
// xrSuggestInteractionProfileBindings refuse the whole controller's bindings
static void AddSuggestedBinding(std::vector<XrActionSuggestedBinding>& bindings, XrAction action, XrPath path)
{
	if (action == XR_NULL_HANDLE || path == XR_NULL_PATH)
		return;
	bindings.push_back(XrActionSuggestedBinding{ action, path });
}

static std::string dirnameOf(const std::string& fname)
{
	size_t pos = fname.find_last_of("\\/");
	return (std::string::npos == pos)
	    ? ""
	    : fname.substr(0, pos);
}

// ASCII-only to-lower-case - for use with strings in maps, since steamvr is case-independent (In which
// locale you might wonder? Good question).
static std::string lowerStr(const std::string& in)
{
	std::string out;
	out.reserve(in.size());

	for (char c : in) {
		if (c >= 'A' && c <= 'Z') {
			c = (char)(c - 'A' + 'a');
		}
		out.push_back(c);
	}

	return out;
}

static void stringSplit(const std::string& str, std::vector<std::string>& items)
{
	items.clear();

	for (size_t i = 0; i < str.size();) {
		size_t nextStroke = str.find('/', i);

		// If the path starts with a stroke, ignore it - that's a very common case
		if (nextStroke == 0) {
			i++;
			continue;
		}

		// No more strokes in the string? This is the last bit
		if (nextStroke == std::string::npos) {
			items.push_back(str.substr(i));
			break;
		}

		items.push_back(str.substr(i, nextStroke - i));

		// Skip to one past the next stroke
		i = nextStroke + 1;
	}
}

// Converts an arbitrary application-supplied string to one we can use with OpenXR
static std::string escapePathString(const std::string& str)
{
	std::vector<char> out;
	out.reserve(str.length() + 10);
	for (char c : str) {
		// Valid list of characters, see the OpenXR spec part 6.2
		if (('a' <= c && c <= 'z') || ('0' <= c && c <= '9') || c == '-' || c == '_' || c == '.' || c == '/') {
			out.push_back(c);
			continue;
		}

		// Special-case uppercase letters by prefixing them with an underscore and lowering them
		// This will nicely convert CamelCase to snake_case, though THIS_CASE becomes ugly.
		if ('A' <= c && c <= 'Z') {
			out.push_back('_');
			out.push_back(c - 'A' + 'a');
			continue;
		}

		// Just print out the hex code
		char hex[3];
		snprintf(hex, sizeof(hex), "%02x", c);
		out.push_back('.');
		out.push_back(hex[0]);
		out.push_back(hex[1]);
	}

	return std::string(out.data(), out.size());
}

// Must be defined non-inline to avoid it ending up in stubs.gen.cpp
BaseInput::Action::~Action() = default;
// The input value handles GetInputSourceHandle has handed out (they're pointers): cast_IVH answers null for any
// other value instead of reading through it. Never freed: ~InputValueHandle can run during static destruction (the
// interfaces map in openvr_api.cpp keeps BaseInput alive until then)
static std::mutex& LiveHandlesLock()
{
	static std::mutex& lock = *new std::mutex;
	return lock;
}
static std::unordered_set<const void*>& LiveHandles()
{
	static std::unordered_set<const void*>& handles = *new std::unordered_set<const void*>;
	return handles;
}

BaseInput::InputValueHandle::InputValueHandle()
{
	std::lock_guard<std::mutex> guard(LiveHandlesLock());
	LiveHandles().insert(this);
}
BaseInput::InputValueHandle::~InputValueHandle()
{
	std::lock_guard<std::mutex> guard(LiveHandlesLock());
	LiveHandles().erase(this);
}

// Registry implementation

template <typename T>
BaseInput::Registry<T>::Registry(uint32_t _maxNameSize)
    : maxNameSize(_maxNameSize) {}

template <typename T>
T* BaseInput::Registry<T>::LookupItem(const std::string& name) const
{
	auto iter = itemsByName.find(lowerStr(name));
	if (iter == itemsByName.end())
		return nullptr;
	return iter->second;
}

template <typename T>
T* BaseInput::Registry<T>::LookupItem(RegHandle handle) const
{
	auto iter = itemsByHandle.find(handle);
	if (iter == itemsByHandle.end())
		return nullptr;
	return iter->second;
}

template <typename T>
BaseInput::RegHandle BaseInput::Registry<T>::LookupHandle(const std::string& name)
{
	std::lock_guard<std::mutex> guard(nameLock);
	std::string lowerName = ShortenOrLookupNameLocked(name);
	auto iter = handlesByName.find(lowerName);
	if (iter != handlesByName.end())
		return iter->second;

	// Create a new dummy handle. It's only in the handlesByName map, so looking up the item
	// for it will return nullptr.
	RegHandle handle = 0xabcd0001 + handlesByName.size();
	handlesByName[lowerName] = handle;
	namesByHandle[handle] = name;
	return handle;
}

template <typename T>
std::string BaseInput::Registry<T>::ShortenOrLookupName(const std::string& longName)
{
	std::lock_guard<std::mutex> guard(nameLock);
	return ShortenOrLookupNameLocked(longName);
}

template <typename T>
std::string BaseInput::Registry<T>::ShortenOrLookupNameLocked(const std::string& longName)
{
	std::string ret = lowerStr(longName);
	if (ret.size() > maxNameSize - 1) {
		auto iter = longNames.find(ret);
		if (iter == longNames.end()) {
			// new name - shorten and append "_ln" + unique number (ln for Long Name)
			std::string fullName = ret;
			std::string unique_id = "_ln" + std::to_string(longNames.size());
			ret = ret.substr(0, maxNameSize - 1 - unique_id.size()) + unique_id;
			longNames[fullName] = ret;
			OOVR_LOGF("Shortened name %s to %s", fullName.c_str(), ret.c_str());
		} else {
			// name has already been shortened before - return the shortened version. It may have no handle yet, if
			// nothing was made with it: an action the manifest parser skips, or a binding's action not in the manifest.
			return iter->second;
		}
	}
	return ret;
}

template <typename T>
T* BaseInput::Registry<T>::Initialise(const std::string& name, std::unique_ptr<T> value)
{
	std::lock_guard<std::mutex> guard(nameLock);
	std::string lowerName = lowerStr(name);
	RegHandle handle = 0;

	// apparently games CAN in fact grab handles before initialization - Kayak VR does this
	// grab already created handle if it exists
	if (handlesByName.count(lowerName) != 0) {
		// since we only generate dummy handles before initialization, make sure we only have dummy handles
		// dummy handles have no associated items
		handle = handlesByName.find(lowerName)->second;
		if (itemsByHandle.find(handle) != itemsByHandle.end()) {
			// Can't happen (an item is only made once per name); if it does, the newer item replaces the older
			OOVR_LOGF("Input registry: '%s' initialised twice", lowerName.c_str());
		}
	}

	// Move the pointer into storage, so we'll own it
	T* ptr = value.get();
	storage.emplace_back(std::move(value));

	// Use pointer as handle, for ease of debugging only
	// Since our values live as long as the registry, it's guaranteed their addresses won't repeat
	if (!handle) {
		handle = (RegHandle)ptr;
		handlesByName[lowerName] = handle;
		namesByHandle[handle] = lowerName;
	}

	itemsByName[lowerName] = ptr;
	itemsByHandle[handle] = ptr;

	// Convenience return
	return ptr;
}

template <typename T>
void BaseInput::Registry<T>::Reset()
{
	// We want to preserve handlesByName and namesByHandle, because handles are supposed to always be accessible
	// from the same values regardless of if said handles are actually currently valid
	// In the case of NomaiVR, it will set an action manifest, get all the action handles, and then set another (identical) manifest
	// We can clear the actual item storage though, since these will no longer be valid
	itemsByHandle.clear();
	itemsByName.clear();
	storage.clear();
}

template class BaseInput::Registry<BaseInput::Action>;
template class BaseInput::Registry<BaseInput::ActionSet>;

// ---

static double EvaluatorClock(); // below, with the evaluator

BaseInput::BaseInput()
    : actionSets(XR_MAX_ACTION_SET_NAME_SIZE), actions(XR_MAX_ACTION_NAME_SIZE)
{
	// Initialise the subaction path constants
	for (const std::string& str : allSubactionPathNames) {
		XrPath path = XR_NULL_PATH;
		OC_XR_LOG(xrStringToPath(xr_instance, str.c_str(), &path));
		allSubactionPaths.push_back(path);
		handInteractionProfiles.push_back(nullptr);
	}

	if (!startupManifest.empty()) {
		SetActionManifestPath(startupManifest.c_str());
	}
}
BaseInput::~BaseInput()
{
	for (XrHandTrackerEXT& handTracker : handTrackers) {
		if (handTracker != XR_NULL_HANDLE && xr_ext)
			xr_ext->xrDestroyHandTrackerEXT(handTracker);
		handTracker = XR_NULL_HANDLE;
	}
}

void BaseInput::setStartupManifest(std::string manifest)
{
	startupManifest = manifest;
	if (auto input = GetUnsafeBaseInput(); input) {
		input->SetActionManifestPath(startupManifest.c_str());
	}
}

EVRInputError BaseInput::SetActionManifestPath(const char* pchActionManifestPath)
{
	if (!pchActionManifestPath || !*pchActionManifestPath)
		return VRInputError_InvalidParam;

	// It stands to reason that nothing would happen if we try to load the same manifest again
	{
		std::lock_guard<std::mutex> pathGuard(loadedActionsPathLock);
		if (hasLoadedActions && loadedActionsPath == pchActionManifestPath)
			return vr::VRInputError_None;
	}
	OOVR_LOGF("Loading manifest file '%s'", pchActionManifestPath);

	//////////////
	//// Load the actions from the manifest file
	//////////////

	// Read it first, while other threads still read input: a manifest that can't be read leaves the game on legacy
	// input (and any manifest it had), and answers InvalidParam as SteamVR does for a path it can't use
	Json::Value root;
	std::string manifestError;
	if (!ReadJson(pchActionManifestPath, root, manifestError) || !root.isObject()) {
		OOVR_LOGF("Failed to open or parse input manifest: %s", manifestError.empty() ? "not a JSON object" : manifestError.c_str());
		return VRInputError_InvalidParam;
	}

	// Exclusive to the end: the old actions go, and the new ones are made and bound to the restarted session, while
	// no other thread reads input. Taken before evaluatorLock (see its declaration).
	auto sessionLock = xr_session.lock();

	// While a lost instance is made again: this manifest is the one loaded on the new instance
	if (rebuildPending) {
		manifestToRebuild = pchActionManifestPath;
		rebuildLegacy = false;
		return vr::VRInputError_None;
	}

	bool reloading = hasLoadedActions;
	if (hasLoadedActions) {
		// It stands to reason that nothing would happen if we try to load the same manifest again
		if (loadedActionsPath == pchActionManifestPath)
			return vr::VRInputError_None;

		OOVR_LOG("Received another manifest! Restarting session to reattach inputs...");
		DropActions(true);
	}

	restartingSession = true;
	XrBackend::MaybeRestartForInputs();
	restartingSession = false;

	// The evaluators' state is filled from here on, under evaluatorLock as everywhere it's read
	std::lock_guard<std::recursive_mutex> loadGuard(evaluatorLock);

	hasLoadedActions = true;
	{
		std::lock_guard<std::mutex> pathGuard(loadedActionsPathLock);
		loadedActionsPath = pchActionManifestPath;
	}

	// Another manifest in place of the first: its bindings are new
	if (reloading) {
		OcPostEvent(vr::VREvent_ActionBindingReloaded, vr::k_unTrackedDeviceIndexInvalid, vr::VREvent_Data_t{}, false);
	}

	// Random setting which is in the action manifest
	const Json::Value& dominantHandSetting = root["supports_dominant_hand_setting"];
	allowSetDominantHand = dominantHandSetting.isConvertibleTo(Json::booleanValue) && dominantHandSetting.asBool();

	// Parse the actions
	OOVR_LOG("Parsing actions...");
	std::set<std::string> skippedActionSets; // the sets of actions we leave out, which the app still syncs
	for (Json::Value item : root["actions"]) try {
		if (!item.isObject()) {
			OOVR_LOG("Warning: skipping an action that isn't a JSON object");
			continue;
		}
		std::unique_ptr<Action> actionPtr = std::make_unique<Action>();
		Action& action = *actionPtr;
		action.createdAt = EvaluatorClock();

		action.fullName = actions.ShortenOrLookupName(item["name"].asString());
		action.manifestName = lowerStr(item["name"].asString());

		// Split the full name by stroke ('/') characters
		// They have four parts: the first is always 'actions', the second is the action set name, the third is
		// either 'in' or 'out' (in for inputs, out for haptics) and the last is the action's short name.
		std::vector<std::string> parts;
		stringSplit(action.fullName, parts);

		// A bad entry is skipped with a warning; the rest of the manifest is used
		if (parts.size() != 4) {
			OOVR_LOGF("Warning: skipping action '%s' - wrong number of parts %zu", action.fullName.c_str(), parts.size());
			continue;
		}

		if (parts.at(0) != "actions") {
			OOVR_LOGF("Warning: skipping action '%s' - bad first part '%s'", action.fullName.c_str(), parts.at(0).c_str());
			continue;
		}

		action.setName = parts.at(1);
		action.shortName = parts.at(3);

		std::string inOut = parts.at(2);
		if (inOut == "in")
			action.haptic = false;
		else if (inOut == "out")
			action.haptic = true;
		else {
			OOVR_LOGF("Warning: skipping action '%s' - bad in/out value '%s'", action.fullName.c_str(), inOut.c_str());
			continue;
		}

		// Parse the type
		std::string type = item["type"].asString();
		if (type == "boolean")
			action.type = ActionType::Boolean;
		else if (type == "vector1")
			action.type = ActionType::Vector1;
		else if (type == "vector2")
			action.type = ActionType::Vector2;
		else if (type == "vector3")
			action.type = ActionType::Vector3; // (x, y, 0) from a two-axis source
		else if (type == "vibration")
			action.type = ActionType::Vibration;
		else if (type == "pose")
			action.type = ActionType::Pose;
		else if (type == "skeleton")
			action.type = ActionType::Skeleton;
		else if (type == "eyetracking")
			action.type = ActionType::EyeTracking;
		else {
			OOVR_LOGF("Warning: skipping action '%s' of unknown type '%s'", action.fullName.c_str(), type.c_str());
			skippedActionSets.insert(action.setName);
			continue;
		}

		if (actions.LookupItem(action.fullName) != nullptr) {
			OOVR_LOGF("Warning: skipping duplicate action '%s'", action.fullName.c_str());
			continue;
		}

		// If this is a skeletal action, see what hand it's bound to. Uniquely, this is done in the actions manifest itself
		if (action.type == ActionType::Skeleton) {
			// An action that names no hand, or one other than the two, is left out: it reads as an invalid skeleton
			std::string skelSide = lowerStr(item["skeleton"].asString());
			if (skelSide == "/skeleton/hand/left") {
				action.skeletalHand = ITrackedDevice::HAND_LEFT;
			} else if (skelSide == "/skeleton/hand/right") {
				action.skeletalHand = ITrackedDevice::HAND_RIGHT;
			} else {
				OOVR_LOGF("Warning: skipping skeletal action '%s' - its hand side '%s' isn't /skeleton/hand/left or right",
				    action.fullName.c_str(), skelSide.c_str());
				skippedActionSets.insert(action.setName);
				continue;
			}
		}

		std::string name = action.fullName; // Array index may be evaluated first iirc, so pull this out now
		actions.Initialise(name, std::move(actionPtr));
	} catch (const std::exception& e) {
		// A wrong-typed value in this entry (Json::LogicError): only the entry is left out
		OOVR_SOFT_ABORTF("Skipping an action of the manifest: %s", e.what());
	}

	// Parse the action sets
	OOVR_LOG("Parsing action sets...");
	for (Json::Value item : root["action_sets"]) try {
		if (!item.isObject()) {
			OOVR_LOG("Warning: skipping an action set that isn't a JSON object");
			continue;
		}
		ActionSet set = {};

		set.fullName = actionSets.ShortenOrLookupName(item["name"].asString());
		set.manifestName = lowerStr(item["name"].asString());

		// Split the full name by stroke ('/') characters
		// They have four parts: the first is always 'actions', the second is the action set name, the third is
		// either 'in' or 'out' (in for inputs, out for haptics) and the last is the action's short name.
		std::vector<std::string> parts;
		stringSplit(set.fullName, parts);

		if (parts.size() != 2) {
			OOVR_LOGF("Warning: skipping action set '%s' - wrong number of parts %zu", set.fullName.c_str(), parts.size());
			continue;
		}

		if (parts.at(0) != "actions") {
			OOVR_LOGF("Warning: skipping action set '%s' - bad first part '%s'", set.fullName.c_str(), parts.at(0).c_str());
			continue;
		}
		if (actionSets.LookupItem(set.fullName) != nullptr) {
			OOVR_LOGF("Warning: skipping duplicate action set '%s'", set.fullName.c_str());
			continue;
		}

		set.name = parts.at(1);

		// Find the usage
		std::string usage = item["usage"].asString();
		if (usage == "leftright")
			set.usage = ActionSetUsage::LeftRight;
		else if (usage == "single")
			set.usage = ActionSetUsage::Single;
		else if (usage == "hidden")
			set.usage = ActionSetUsage::Hidden;
		else {
			OOVR_LOGF("Warning: invalid usage '%s' for action set '%s', using leftright", usage.c_str(), set.name.c_str());
			set.usage = ActionSetUsage::LeftRight;
		}

		// Register it
		actionSets.Initialise(set.fullName, std::make_unique<ActionSet>(set));
	} catch (const std::exception& e) {
		OOVR_SOFT_ABORTF("Skipping an action set of the manifest: %s", e.what());
	}

	// Make sure all the actions have a corresponding set
	// In the OpenVR samples, they don't declare their action set. Are they automatically created?!
	// Also Vivecraft unfortunately does this, so we do have to support it.
	auto findOrCreateSet = [&](const std::string& setName, const std::string& forAction) {
		std::string fullName = "/actions/" + setName;
		ActionSet* set = actionSets.LookupItem(fullName);
		if (set == nullptr) {
			OOVR_LOGF("Invalid action set '%s' for action '%s', creating implicit set", setName.c_str(), forAction.c_str());

			// Create this set
			set = actionSets.Initialise(fullName, std::make_unique<ActionSet>());
			set->name = setName;
			set->fullName = fullName;
			set->manifestName = lowerStr(fullName);
			set->usage = ActionSetUsage::LeftRight; // Just assume these default sets are in leftright mode, FIXME validate against steamvr
		}
		return set;
	};
	for (const std::unique_ptr<Action>& action : actions.GetItems()) {
		action->set = findOrCreateSet(action->setName, action->fullName);
	}
	for (const std::string& setName : skippedActionSets) {
		findOrCreateSet(setName, "(skipped)");
	}

	//////////////////////////
	/// Now we've got everything done, load the actions into OpenXR
	//////////////////////////

	// The names the manifest gives for the binding UI; OpenXR wants the ones of a kind (action sets, an action set's
	// actions) different, so a name already used falls back to the short one
	const std::map<std::string, std::string> localized = ReadManifestLocalization(root, PathFromUtf8(dirnameOf(pchActionManifestPath)), "en_US");
	auto localizedName = [&](const std::string& manifestName, const std::string& fallback, std::set<std::string>& used) {
		auto it = localized.find(manifestName);
		if (it != localized.end() && used.insert(it->second).second)
			return it->second;
		used.insert(fallback);
		return fallback;
	};

	std::set<std::string> usedSetNames;
	for (const std::unique_ptr<ActionSet>& as : actionSets.GetItems()) {
		XrActionSetCreateInfo createInfo = { XR_TYPE_ACTION_SET_CREATE_INFO };
		std::string safeName = escapePathString(as->name);
		strcpy_arr(createInfo.actionSetName, safeName.c_str());
		strcpy_arr(createInfo.localizedActionSetName, localizedName(as->manifestName, as->name, usedSetNames).c_str());

		OC_XR_LOG(xrCreateActionSet(xr_instance, &createInfo, &as->xr));
	}

	std::map<const ActionSet*, std::set<std::string>> usedActionNames;
	for (const std::unique_ptr<Action>& act : actions.GetItems()) {
		// Only a vibration has an OpenXR action of its own: the other types are read from the raw inputs, and a pose,
		// a skeleton and the eye tracking come from the legacy set
		if (act->type != ActionType::Vibration)
			continue;

		XrActionCreateInfo info = { XR_TYPE_ACTION_CREATE_INFO };
		std::string safeName = escapePathString(act->shortName);
		strcpy_arr(info.actionName, safeName.c_str());
		strcpy_arr(info.localizedActionName, localizedName(act->manifestName, act->shortName, usedActionNames[act->set]).c_str());
		info.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;

		// Listen on all the subactions
		info.subactionPaths = allSubactionPaths.data();
		info.countSubactionPaths = allSubactionPaths.size();

		OC_XR_LOG(xrCreateAction(act->set->xr, &info, &act->xr));
	}

	CreateLegacyActions();
	CreateRawActions();

	// load all available bindings for known interaction profiles
	OOVR_LOG("Loading known bindings...");
	// Map of OpenVR names (i.e. "vive_controller") to interaction profile pointers: one name may have several profiles
	// (both Pico controllers are pico_controller)
	std::unordered_map<std::string, std::vector<InteractionProfile*>> OpenVRNameToProfile;
	for (const std::unique_ptr<InteractionProfile>& profile : InteractionProfile::GetProfileList()) {
		if (profile->GetOpenVRName().has_value())
			OpenVRNameToProfile[profile->GetOpenVRName().value()].push_back(profile.get());
		else
			// Add any unrecognized profiles (simple controller) to the end so it still gets assigned a profile
			OpenVRNameToProfile[profile->GetPath()].push_back(profile.get());
	}

	// For determining the best path for supported controllers that don't have a profile handled by the game (i.e. khr/simple_controller)
	std::string backupPath;
	int8_t priority = -1;

	// these priorities are a bit arbitrary
	const std::unordered_map<std::string, uint8_t> typeToPriority = {
		{ "oculus_touch", 3 },
		{ "knuckles", 2 },
		{ "generic", 1 }
	};

	// The user's own bindings first: their folder, the OpenComposite one it replaced, then the game's working folder's OpenLamina and OpenComposite folders, then
	// the game's default
	const std::string appKey = CurrentProcessAppKey();
	const std::filesystem::path userBindings = UserBindingsDir();
	const std::filesystem::path legacyUserBindings = LegacyUserBindingsDir();
	std::error_code cwdError;
	const std::filesystem::path workingDir = std::filesystem::current_path(cwdError);
	const std::filesystem::path manifestDir = PathFromUtf8(dirnameOf(pchActionManifestPath));

	trackerPoseRoutes.clear();
	for (Json::Value item : root["default_bindings"]) try {
		if (!item.isObject()) {
			OOVR_LOG("Warning: skipping a default_bindings entry that isn't a JSON object");
			continue;
		}
		std::string controller_type = item["controller_type"].asString();
		std::string path = dirnameOf(pchActionManifestPath) + "/" + item["binding_url"].asString();

		std::optional<std::filesystem::path> found = FindBindingFile(appKey, controller_type, userBindings, workingDir, manifestDir,
		    item["binding_url"].asString(), legacyUserBindings);
		if (found && found->lexically_normal() != (manifestDir / PathFromUtf8(item["binding_url"].asString())).lexically_normal()) {
			OOVR_LOGF("Loading the user's own bindings for controller type %s: %s", controller_type.c_str(), PathToUtf8(*found).c_str());
			path = PathToUtf8(*found);
		}

		// A tracker's binding file: the poses it binds on a tracker's path (/user/waist/pose/raw) are that tracker's
		if (controller_type.starts_with("vive_tracker")) {
			const BindingFile tracker = LoadBindingFile(PathFromUtf8(path));
			for (const auto& [setName, set] : tracker.sets)
				for (const PoseBinding& pose : set.poses)
					if (std::optional<ViveTrackerRole> role = ViveTrackerRoleForSteamVRPath(pose.path)) {
						std::vector<std::string>& roles = trackerPoseRoutes[pose.output];
						if (std::find(roles.begin(), roles.end(), role->name) == roles.end())
							roles.push_back(role->name);
					}
		}

		// look if we know about this OpenVR name
		// have to loop through every binding type because default_bindings is an array for some reason
		auto iter = OpenVRNameToProfile.find(controller_type);
		if (iter != OpenVRNameToProfile.end()) {
			for (InteractionProfile* profile : iter->second)
				LoadBindingsSet(*profile, path);
			// profile has been bound: we don't need it in our map anymore
			OpenVRNameToProfile.erase(iter);
		}

		// get priority for remaining unbound profiles
		auto iter2 = typeToPriority.find(controller_type);
		if (iter2 != typeToPriority.end()) {
			if (iter2->second > priority) {
				priority = iter2->second;
				backupPath = path;
			}
		} else if (priority < 0) {
			priority = 0;
			backupPath = path;
		}
	} catch (const std::exception& e) {
		OOVR_SOFT_ABORTF("Skipping a default_bindings entry of the manifest: %s", e.what());
	}

	// remaining profiles: the user's own bindings for them if there are any, else our backup path
	for (auto& [name, profiles] : OpenVRNameToProfile) {
		std::optional<std::filesystem::path> own = FindBindingFile(appKey, name, userBindings, workingDir, {}, "", legacyUserBindings);
		if (own)
			OOVR_LOGF("Loading the user's own bindings for controller type %s: %s", name.c_str(), PathToUtf8(*own).c_str());
		for (InteractionProfile* profile : profiles)
			LoadBindingsSet(*profile, own ? PathToUtf8(*own) : backupPath);
	}

	// Attach everything to the current session
	BindInputsForSession();
	return vr::VRInputError_None;
}

void BaseInput::DropActions(bool destroyHandles)
{
	std::lock_guard<std::recursive_mutex> evalGuard(evaluatorLock);
	if (destroyHandles) {
		for (std::unique_ptr<ActionSet>& as : actionSets.GetItems()) {
			OC_XR_LOG(xrDestroyActionSet(as->xr));
		}
		if (legacyInputsSet != XR_NULL_HANDLE)
			OC_XR_LOG(xrDestroyActionSet(legacyInputsSet));
		if (rawInputsSet != XR_NULL_HANDLE)
			OC_XR_LOG(xrDestroyActionSet(rawInputsSet));
	}
	legacyInputsSet = XR_NULL_HANDLE;
	eyeGazeAction = XR_NULL_HANDLE;
	eyeGazeSpace = XR_NULL_HANDLE;
	trackerPoseAction = XR_NULL_HANDLE;
	trackerPoseSession = XR_NULL_HANDLE;
	rawInputsSet = XR_NULL_HANDLE;
	rawActions.clear();
	rawComponents.clear();
	evaluators.clear();
	bindingFiles.clear();
	aliasedBindingFiles.clear();
	evaluatedThisSync.clear();
	legacyEvaluators.clear();
	{
		std::lock_guard<std::mutex> queueGuard(hapticQueueLock);
		pendingHaptics.clear();
	}
	actions.Reset();
	actionSets.Reset();
	usingLegacyInput = false;
}

void BaseInput::_ForgetInstance()
{
	auto sessionLock = xr_session.lock();
	// Paths are the instance's own too: looked up again on the new one
	pathsStale = true;
	// Until the new instance has a session, the game's input calls only say what to load on it
	if (rebuildPending)
		return; // forgotten at an earlier try
	rebuildPending = true;
	rebuildLegacy = false;
	manifestToRebuild.clear();
	if (!hasLoadedActions)
		return;
	{
		std::lock_guard<std::mutex> pathGuard(loadedActionsPathLock);
		manifestToRebuild = loadedActionsPath;
		loadedActionsPath.clear();
	}
	rebuildLegacy = manifestToRebuild.empty();
	// Their handles went with the old instance: forgotten, not destroyed
	DropActions(false);
	for (XrHandTrackerEXT& handTracker : handTrackers)
		handTracker = XR_NULL_HANDLE;
	hasLoadedActions = false;
}

void BaseInput::_RebuildForNewInstance()
{
	// Exclusive to the end, as a manifest's loading is
	auto sessionLock = xr_session.lock();
	if (pathsStale) {
		pathsStale = false;
		for (size_t i = 0; i < allSubactionPathNames.size() && i < allSubactionPaths.size(); i++) {
			allSubactionPaths[i] = XR_NULL_PATH;
			OC_XR_LOG(xrStringToPath(xr_instance, allSubactionPathNames[i].c_str(), &allSubactionPaths[i]));
		}
		std::fill(handInteractionProfiles.begin(), handInteractionProfiles.end(), nullptr);
		std::lock_guard<std::mutex> registryGuard(inputHandleRegistryLock);
		for (auto& [name, handle] : inputHandleRegistry) {
			handle->devicePath = XR_NULL_PATH;
			if (!handle->devicePathString.empty())
				OC_XR_LOG(xrStringToPath(xr_instance, handle->devicePathString.c_str(), &handle->devicePath));
		}
	}
	if (!rebuildPending)
		return;
	rebuildPending = false;
	if (manifestToRebuild.empty() && !rebuildLegacy)
		return; // the game had no actions
	OOVR_LOG("Making the game's actions again on the new OpenXR instance");
	if (!manifestToRebuild.empty()) {
		if (EVRInputError err = SetActionManifestPath(manifestToRebuild.c_str()); err == vr::VRInputError_None)
			return;
		else
			OOVR_LOGF("The game's manifest couldn't be loaded again (%d): legacy input instead", (int)err);
	}
	LoadEmptyManifestIfRequired();
}

void BaseInput::LoadEmptyManifestIfRequired()
{
	if (hasLoadedActions)
		return;
	// Asked for, legacy input comes in place of the info set (which would cost a session restart)
	legacyInputAsked = true;
	// Not before the game's first frame on its own device: the session restart for it would come anyway, and a
	// manifest the game loads by then would need another
	if (!XrBackend::InputsMayAttach())
		return;

	// As in SetActionManifestPath, exclusive to the end; another thread may have loaded the actions while this waited.
	// Nor while a lost instance is made again: the actions are made on the new one.
	auto sessionLock = xr_session.lock();
	if (rebuildPending && manifestToRebuild.empty())
		rebuildLegacy = true;
	if (hasLoadedActions || rebuildPending)
		return;

	restartingSession = true;
	XrBackend::MaybeRestartForInputs();
	restartingSession = false;

	// As in SetActionManifestPath: the evaluators' state is filled under evaluatorLock, after the restart
	std::lock_guard<std::recursive_mutex> loadGuard(evaluatorLock);

	OOVR_LOG("Loading virtual empty manifest");

	hasLoadedActions = true;

	// We have to set this after calling BindInputsForSession, as otherwise BaseCompositor::Submit might
	// be called concurrently, and use the inputs we haven't set properly.
	// See issue #408.
	usingLegacyInput = false;

	// TODO deduplicate with the regular manifest loader
	CreateLegacyActions();

	// The old input as SteamVR's legacy binding files map it, from the raw inputs
	CreateRawActions();

	// Load in the suggested bindings for the legacy input actions
	for (const std::unique_ptr<InteractionProfile>& profile : InteractionProfile::GetProfileList()) {
		if (!profile->CanHaveBindings())
			continue;

		std::vector<XrActionSuggestedBinding> bindings;
		for (const auto& legacyController : legacyControllers) {
			profile->AddLegacyBindings(legacyController, bindings);
		}
		SuggestRawInputs(*profile, bindings);

		// Load the bindings into the runtime
		XrPath interactionProfilePath = XR_NULL_PATH;
		OC_XR_LOG(xrStringToPath(xr_instance, profile->GetPath().c_str(), &interactionProfilePath));
		XrInteractionProfileSuggestedBinding suggestedBindings{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };

		suggestedBindings.interactionProfile = interactionProfilePath;
		suggestedBindings.suggestedBindings = bindings.data();
		suggestedBindings.countSuggestedBindings = bindings.size();
		OC_XR_LOG(SuggestBindings(suggestedBindings, bindings));
	}

	// Attach everything to the current session
	BindInputsForSession();

	// Finally mark the inputs as being set up
	usingLegacyInput = true;
}

void BaseInput::BindInputsForSession()
{
	// Not attached to this session (yet)
	trackerPoseSession = XR_NULL_HANDLE;

	// This is called from DrvOpenXR::SetupSession. If we requested a session restart ourselves, we're also
	// going to bind the inputs ourselves anyway, so we don't want to do that twice.
	// If we didn't request the restart but we also haven't even loaded actions yet, there's nothing to bind yet.
	if (restartingSession || !hasLoadedActions)
		return;
	// No session to bind to (a lost one was destroyed): the one made again binds them
	if (!xr_gbl || !xr_session.get())
		return;

	// Since the session has changed, any actionspaces we previously created are now invalid
	for (const std::unique_ptr<Action>& action : actions.GetItems()) {
		action->actionSpaces.clear();
	}

	// Same goes for the actionspaces of the legacy controller pose actions, this time create
	// new ones for this session.
	for (LegacyControllerActions& lca : legacyControllers) {
		// No need to destroy it, the session it was attached to was destroyed
		lca.gripPoseSpace = XR_NULL_HANDLE;
		lca.aimPoseSpace = XR_NULL_HANDLE;

		// Create the new spaces
		XrActionSpaceCreateInfo info = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
		info.poseInActionSpace = S2O_om34_pose(G2S_m34(glm::identity<glm::mat4>()));
		info.action = lca.gripPoseAction;
		OC_XR_LOG(xrCreateActionSpace(xr_session.get(), &info, &lca.gripPoseSpace));
		info.action = lca.aimPoseAction;
		OC_XR_LOG(xrCreateActionSpace(xr_session.get(), &info, &lca.aimPoseSpace));
	}

	eyeGazeSpace = XR_NULL_HANDLE;
	if (eyeGazeAction != XR_NULL_HANDLE) {
		XrActionSpaceCreateInfo info = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
		info.poseInActionSpace = S2O_om34_pose(G2S_m34(glm::identity<glm::mat4>()));
		info.action = eyeGazeAction;
		OC_XR_LOG(xrCreateActionSpace(xr_session.get(), &info, &eyeGazeSpace));
	}

	// Note: even if actionSets is empty, we always still want to load the legacy set.

	// Now attach the action sets to the OpenXR session, making them immutable (including attaching suggested bindings)
	std::vector<XrActionSet> sets;
	for (const std::unique_ptr<ActionSet>& as : actionSets.GetItems()) {
		sets.push_back(as->xr);
	}

	sets.push_back(legacyInputsSet);
	if (rawInputsSet != XR_NULL_HANDLE)
		sets.push_back(rawInputsSet);

	// A set OpenXR refused to create is left out, or attaching would fail for all of them
	sets.erase(std::remove(sets.begin(), sets.end(), (XrActionSet)XR_NULL_HANDLE), sets.end());
	XrSessionActionSetsAttachInfo attachInfo = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	attachInfo.actionSets = sets.data();
	attachInfo.countActionSets = sets.size();
	const XrResult attached = xrAttachSessionActionSets(xr_session.get(), &attachInfo);
	OC_XR_LOG(attached);
	if (XR_SUCCEEDED(attached) && trackerPoseAction != XR_NULL_HANDLE)
		trackerPoseSession = xr_session.get();
	// The trackers by role connected before the attach get their spaces now
	if (TrackerPoseAction() != XR_NULL_HANDLE)
		if (auto* backend = dynamic_cast<XrBackend*>(BackendManager::Instance().GetBackendInstance()))
			backend->OnTrackerActionAttached();

	// Setup hand tracking if supported
	if (xr_gbl->handTrackingProperties.supportsHandTracking) {
		XrHandTrackerCreateInfoEXT createInfo = { XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT };
		for (int i = 0; i < 2; i++) {
			createInfo.hand = i == 0 ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
			createInfo.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
			OC_XR_LOG(xr_ext->xrCreateHandTrackerEXT(xr_session.get(), &createInfo, &handTrackers[i]));
		}
	}
}

void BaseInput::LoadBindingsSet(const InteractionProfile& profile, const std::string& bindingsPath)
{
	OOVR_LOGF("Loading bindings for %s", profile.GetPath().c_str());

	if (!profile.CanHaveBindings())
		return;

	// A binding file that can't be used leaves this controller type without bindings; the others still load
	Json::Value bindingsRoot;
	std::string bindingsError;
	if (!ReadJson(bindingsPath, bindingsRoot, bindingsError) || !bindingsRoot.isObject()) {
		OOVR_LOGF("Warning: can't read the binding file, no bindings for %s: %s", profile.GetPath().c_str(),
		    bindingsError.empty() ? "not a JSON object" : bindingsError.c_str());
		return;
	}

	// TODO aliases, if anyone uses them
	if (!bindingsRoot["alias_info"].empty())
		OOVR_LOGF("WARNING: Ignoring alias_info from binding descriptor %s", bindingsPath.c_str());

	const Json::Value& bindingsJson = bindingsRoot["bindings"];
	if (!bindingsJson.isObject()) {
		OOVR_LOGF("Warning: bindings file %s has no bindings object, no bindings for %s", bindingsPath.c_str(), profile.GetPath().c_str());
		return;
	}

	std::vector<XrActionSuggestedBinding> bindings;

	for (std::string setFullName : bindingsJson.getMemberNames()) try {
		const Json::Value setJson = bindingsJson[setFullName];

		setFullName = lowerStr(setFullName);

		std::string prefix = "/actions/";
		if (strncmp(prefix.c_str(), setFullName.c_str(), prefix.size()) != 0) {
			OOVR_LOGF("Warning: skipping action set '%s' in bindings file '%s' - missing or bad prefix", setFullName.c_str(), bindingsPath.c_str());
			continue;
		}
		std::string setName = setFullName.substr(prefix.size());

		ActionSet* set = actionSets.LookupItem("/actions/" + setName);
		if (set == nullptr) {
			// It seems ActionSets specified in the bindings but not in the action manifest are just ignored.
			// Jet Island has those as of 29/05/2022.
			OOVR_LOGF("WARNING: Missing action set '%s' in bindings file '%s', ignoring", setName.c_str(), bindingsPath.c_str());
			continue;
		}

		for (const auto& item : setJson["poses"]) try {
			if (!item.isObject()) {
				OOVR_LOGF("Warning: skipping a pose binding that isn't a JSON object in bindings file '%s'", bindingsPath.c_str());
				continue;
			}
			std::string specPath = lowerStr(item["path"].asString());

			std::string actionName = lowerStr(item["output"].asString());
			Action* action = actions.LookupItem(actionName);
			if (action == nullptr) {
				OOVR_LOGF("Missing action '%s' in bindings file '%s'", actionName.c_str(), bindingsPath.c_str());
				continue;
			}

			PoseBindingInfo info;

			// Find which hand this is from - on a path like /user/hand/left/pose/raw, after this
			// we're left with pose/raw.
			std::string withoutPrefix = specPath;
			info.hand = ParseAndRemoveHandPrefix(withoutPrefix);
			HandAlias alias = HandAlias::NONE;
			if (info.hand == ITrackedDevice::HAND_NONE) {
				// A hand named by dominance follows the dominant hand when the pose is read
				for (auto [prefix, named] : { std::pair<const char*, HandAlias>{ "/user/hand/primary/", HandAlias::PRIMARY }, { "/user/hand/secondary/", HandAlias::SECONDARY } }) {
					if (withoutPrefix.starts_with(prefix)) {
						withoutPrefix.erase(0, strlen(prefix));
						alias = named;
					}
				}
			}
			if (info.hand == ITrackedDevice::HAND_NONE && alias == HandAlias::NONE) {
				OOVR_LOGF("WARNING: Ignoring invalid pose binding '%s' for %s, bad hand prefix", specPath.c_str(), action->fullName.c_str());
				continue;
			}

			// Parse the paths, matching them up with the components they correspond to
			if (withoutPrefix == "pose/raw" || withoutPrefix == "pose/aim") {
				info.point = PoseBindingPoint::RAW;
			} else if (withoutPrefix == "pose/grip") {
				info.point = PoseBindingPoint::GRIP;
			} else if (withoutPrefix == "pose/handgrip") {
				info.point = PoseBindingPoint::HANDGRIP;
			} else if (withoutPrefix == "pose/base") {
				info.point = PoseBindingPoint::BASE;
			} else if (withoutPrefix == "pose/tip") {
				info.point = PoseBindingPoint::TIP;
			} else if (withoutPrefix == "pose/body") {
				info.point = PoseBindingPoint::BODY;
			} else if (withoutPrefix == "pose/gdc2015") {
				info.point = PoseBindingPoint::GDC2015;
			} else {
				OOVR_LOGF("WARNING: Ignoring unknown pose path '%s' (%s) for action %s", specPath.c_str(), withoutPrefix.c_str(), action->fullName.c_str());
				continue;
			}

			// We only accommodate one binding per hand, since having multiple doesn't make sense - either
			// a hand is tracked or not, since we derive everything from the grip pose.
			if (alias != HandAlias::NONE) {
				(alias == HandAlias::PRIMARY ? action->poseBindingsPrimary : action->poseBindingsSecondary)[&profile] = info;
			} else if (info.hand == ITrackedDevice::HAND_LEFT) {
				action->poseBindingsLeft[&profile] = info;
			} else {
				action->poseBindingsRight[&profile] = info;
			}
		} catch (const std::exception& e) {
			OOVR_SOFT_ABORTF("Skipping a pose binding of bindings file '%s': %s", bindingsPath.c_str(), e.what());
		}

		for (const auto& item : setJson["haptics"]) try {
			if (!item.isObject()) {
				OOVR_LOGF("Warning: skipping a haptic binding that isn't a JSON object in bindings file '%s'", bindingsPath.c_str());
				continue;
			}
			std::string pathStr = lowerStr(item["path"].asString());

			std::string actionName = lowerStr(item["output"].asString());
			Action* action = actions.LookupItem(actionName);
			// A haptic binding for an action the manifest doesn't have, or to a path the controller doesn't have, is skipped
			if (action == nullptr) {
				OOVR_LOGF("Warning: skipping haptic binding for missing action '%s' in bindings file '%s'", actionName.c_str(), bindingsPath.c_str());
				continue;
			}

			// On a hand named by dominance it's suggested on both hands: the evaluator sends it to the dominant one
			std::vector<std::string> paths = { pathStr };
			for (const char* named : { "/user/hand/primary/", "/user/hand/secondary/" }) {
				if (pathStr.starts_with(named)) {
					std::string rest = pathStr.substr(strlen(named));
					paths = { "/user/hand/left/" + rest, "/user/hand/right/" + rest };
				}
			}
			for (const std::string& onHand : paths) {
				if (!profile.IsInputPathValid(onHand)) {
					OOVR_LOGF("Warning: skipping haptic binding to invalid path %s for action %s", onHand.c_str(), actionName.c_str());
					continue;
				}

				XrPath path = XR_NULL_PATH;
				OC_XR_LOG(xrStringToPath(xr_instance, onHand.c_str(), &path));
				AddSuggestedBinding(bindings, action->xr, path);
			}
		} catch (const std::exception& e) {
			OOVR_SOFT_ABORTF("Skipping a haptic binding of bindings file '%s': %s", bindingsPath.c_str(), e.what());
		}
	} catch (const std::exception& e) {
		OOVR_SOFT_ABORTF("Skipping an action set of bindings file '%s': %s", bindingsPath.c_str(), e.what());
	}

	// The raw inputs for this profile, and its binding file's evaluator. Only for a profile the game's bindings give
	// something: OpenLamina's own raw inputs mustn't make the runtime pick a controller the game has no bindings for
	if (!SuggestRawBindings(profile, bindingsPath, bindings))
		return;

	// Only register legacy bindings if this profile is supported by the application. This is to avoid the runtime picking
	// an input profile we know about but the game doesn't (eg khr/simple_controller) over one that the game has actually
	// defined bindings for (eg htc/vive_controller).
	for (const auto& legacyController : legacyControllers) {
		profile.AddLegacyBindings(legacyController, bindings);
	}

	// Load the bindings we just built into the runtime
	XrPath interactionProfilePath = XR_NULL_PATH;
	OC_XR_LOG(xrStringToPath(xr_instance, profile.GetPath().c_str(), &interactionProfilePath));

	XrInteractionProfileSuggestedBinding suggestedBindings{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
	suggestedBindings.interactionProfile = interactionProfilePath;
	suggestedBindings.suggestedBindings = bindings.data();
	suggestedBindings.countSuggestedBindings = bindings.size();
	OC_XR_LOG(SuggestBindings(suggestedBindings, bindings));
}

XrResult BaseInput::SuggestBindings(XrInteractionProfileSuggestedBinding& suggested, std::vector<XrActionSuggestedBinding>& bindings)
{
	XrResult res = xr_suggest_bindings_logged(suggested);
	if (XR_SUCCEEDED(res))
		return res;
	// A runtime needn't offer a controller's system button, and takes a profile's bindings all or none: refused with
	// the dashboard's among them, the rest are suggested again
	const size_t before = bindings.size();
	std::erase_if(bindings, [&](const XrActionSuggestedBinding& b) {
		return b.action != XR_NULL_HANDLE && (b.action == legacyControllers[0].dashboard || b.action == legacyControllers[1].dashboard);
	});
	if (bindings.size() == before || bindings.empty())
		return res;
	OOVR_LOG("The runtime refused the bindings with the system button among them: suggesting them without it (dashboardButton=system won't open the dashboard)");
	suggested.suggestedBindings = bindings.data();
	suggested.countSuggestedBindings = (uint32_t)bindings.size();
	return xr_suggest_bindings_logged(suggested);
}

// With XR_HTCX_vive_tracker_interaction: one pose action for every tracker role, each role a subaction path, bound to
// each role's grip pose. Its profile is the trackers' own (no binding file is loaded for it), so it's suggested here.
static XrAction CreateTrackerPoseAction(XrActionSet set)
{
	if (!xr_ext->ViveTrackersHTCX_Available())
		return XR_NULL_HANDLE;
	std::vector<XrPath> roles;
	std::vector<XrPath> poses;
	for (const ViveTrackerRole& role : ViveTrackerRoles()) {
		XrPath rolePath = XR_NULL_PATH, posePath = XR_NULL_PATH;
		OC_XR_LOG(xrStringToPath(xr_instance, ViveTrackerRolePath(role).c_str(), &rolePath));
		OC_XR_LOG(xrStringToPath(xr_instance, (ViveTrackerRolePath(role) + "/input/grip/pose").c_str(), &posePath));
		if (rolePath == XR_NULL_PATH || posePath == XR_NULL_PATH)
			continue;
		roles.push_back(rolePath);
		poses.push_back(posePath);
	}
	XrActionCreateInfo info = { XR_TYPE_ACTION_CREATE_INFO };
	info.actionType = XR_ACTION_TYPE_POSE_INPUT;
	strcpy_arr(info.actionName, "tracker-pose");
	strcpy_arr(info.localizedActionName, "Tracker Pose");
	info.countSubactionPaths = (uint32_t)roles.size();
	info.subactionPaths = roles.data();
	XrAction action = XR_NULL_HANDLE;
	OC_XR_LOG(xrCreateAction(set, &info, &action));
	if (action == XR_NULL_HANDLE)
		return XR_NULL_HANDLE;

	XrPath profile = XR_NULL_PATH;
	OC_XR_LOG(xrStringToPath(xr_instance, "/interaction_profiles/htc/vive_tracker_htcx", &profile));
	std::vector<XrActionSuggestedBinding> bindings;
	for (XrPath pose : poses)
		bindings.push_back({ action, pose });
	XrInteractionProfileSuggestedBinding suggested = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
	suggested.interactionProfile = profile;
	suggested.suggestedBindings = bindings.data();
	suggested.countSuggestedBindings = (uint32_t)bindings.size();
	OC_XR_LOG(xr_suggest_bindings_logged(suggested));
	return action;
}

void BaseInput::CreateLegacyActions()
{
	// Add the stuff required to make the backend work
	// This means the pose and haptic inputs for each hand, and actions for all the legacy inputs, into a new ActionSet that
	// is always active with a lower priority than the game's one.

	XrActionSetCreateInfo setInfo = { XR_TYPE_ACTION_SET_CREATE_INFO };
	strcpy_arr(setInfo.actionSetName, "openlamina-actions");
	strcpy_arr(setInfo.localizedActionSetName, "OpenLamina Actions");
	setInfo.priority = 0;

	OC_XR_LOG(xrCreateActionSet(xr_instance, &setInfo, &legacyInputsSet));

	for (int i = 0; i < 2; i++) {
		LegacyControllerActions& ctrl = legacyControllers[i];
		ctrl = {};

		std::string side = i == 0 ? "left" : "right";
		ctrl.handPath = "/user/hand/" + side;
		ctrl.handType = i == 0 ? ITrackedDevice::HAND_LEFT : ITrackedDevice::HAND_RIGHT;
		OC_XR_LOG(xrStringToPath(xr_instance, ctrl.handPath.c_str(), &ctrl.handPathXr));

		auto create = [&](XrAction* out, const std::string& name, const std::string& englishName, XrActionType type) {
			XrActionCreateInfo info = { XR_TYPE_ACTION_CREATE_INFO };
			info.actionType = type;

			std::string codeName = "legacy-" + side + "-" + name;
			strcpy_arr(info.actionName, codeName.c_str());

			std::string fullHumanName = "Legacy input: " + englishName + " - " + side; // TODO localisation
			strcpy_arr(info.localizedActionName, fullHumanName.c_str());

			OC_XR_LOG(xrCreateAction(legacyInputsSet, &info, out));
		};

		create(&ctrl.system, "system", "System Button", XR_ACTION_TYPE_BOOLEAN_INPUT);
		create(&ctrl.dashboard, "dashboard", "Dashboard Button", XR_ACTION_TYPE_BOOLEAN_INPUT);
		create(&ctrl.menu, "menu", "Menu Button", XR_ACTION_TYPE_BOOLEAN_INPUT);
		create(&ctrl.menuTouch, "menu-touch", "Menu Button (Touch)", XR_ACTION_TYPE_BOOLEAN_INPUT);
		create(&ctrl.btnA, "btn-a", "'A' Button", XR_ACTION_TYPE_BOOLEAN_INPUT);
		create(&ctrl.btnATouch, "btn-a-touch", "'A' Button (Touch)", XR_ACTION_TYPE_BOOLEAN_INPUT);

		create(&ctrl.stickBtn, "thumbstick-btn", "Thumbstick Button", XR_ACTION_TYPE_BOOLEAN_INPUT);
		create(&ctrl.stickBtnTouch, "thumbstick-btn-touch", "Thumbstick Button (Touch)", XR_ACTION_TYPE_BOOLEAN_INPUT);
		create(&ctrl.stickX, "thumbstick-x", "Thumbstick X axis", XR_ACTION_TYPE_FLOAT_INPUT);
		create(&ctrl.stickY, "thumbstick-y", "Thumbstick Y axis", XR_ACTION_TYPE_FLOAT_INPUT);
		create(&ctrl.trackpadX, "trackpad-x", "Trackpad X axis", XR_ACTION_TYPE_FLOAT_INPUT);
		create(&ctrl.trackpadY, "trackpad-y", "Trackpad Y axis", XR_ACTION_TYPE_FLOAT_INPUT);
		create(&ctrl.trackpadTouch, "trackpad-touch", "Trackpad Touch", XR_ACTION_TYPE_BOOLEAN_INPUT);
		create(&ctrl.trackpadForce, "trackpad-force", "Trackpad Force", XR_ACTION_TYPE_FLOAT_INPUT);

		// Note that while we define the grip as a float, we can still bind it to boolean actions and the OpenXR runtime will
		// return 0.0 or 1.0 depending on the button status. OpenXR 1.0 § 11.4.
		create(&ctrl.grip, "grip", "Grip", XR_ACTION_TYPE_FLOAT_INPUT);
		create(&ctrl.gripClick, "grip-click", "Grip (Digital)", XR_ACTION_TYPE_BOOLEAN_INPUT);
		create(&ctrl.trigger, "trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT);
		create(&ctrl.triggerTouch, "trigger-touch", "Trigger (Touch)", XR_ACTION_TYPE_BOOLEAN_INPUT);
		create(&ctrl.triggerClick, "trigger-click", "Trigger (Digital)", XR_ACTION_TYPE_BOOLEAN_INPUT);
		create(&ctrl.triggerProximity, "trigger-proximity", "Trigger (Finger Near)", XR_ACTION_TYPE_BOOLEAN_INPUT);
		create(&ctrl.thumbProximity, "thumb-proximity", "Thumb Buttons (Thumb Near)", XR_ACTION_TYPE_BOOLEAN_INPUT);
		create(&ctrl.triggerCurl, "trigger-curl", "Trigger (Finger Curl)", XR_ACTION_TYPE_FLOAT_INPUT);

		create(&ctrl.haptic, "haptic", "Vibration Haptics", XR_ACTION_TYPE_VIBRATION_OUTPUT);

		create(&ctrl.gripPoseAction, "grip-pose", "Grip Pose", XR_ACTION_TYPE_POSE_INPUT);
		create(&ctrl.aimPoseAction, "aim-pose", "Aim Pose", XR_ACTION_TYPE_POSE_INPUT);
	}

	// The eye gaze, in the same always-active set; its profile is the eyes' own, so its binding is suggested here
	eyeGazeAction = XR_NULL_HANDLE;
	if (xr_ext->EyeGaze_Available() && xr_gbl && xr_gbl->eyeGazeProperties.supportsEyeGazeInteraction) {
		XrActionCreateInfo info = { XR_TYPE_ACTION_CREATE_INFO };
		info.actionType = XR_ACTION_TYPE_POSE_INPUT;
		strcpy_arr(info.actionName, "eye-gaze");
		strcpy_arr(info.localizedActionName, "Eye Gaze");
		OC_XR_LOG(xrCreateAction(legacyInputsSet, &info, &eyeGazeAction));

		XrPath profile = XR_NULL_PATH, pose = XR_NULL_PATH;
		OC_XR_LOG(xrStringToPath(xr_instance, "/interaction_profiles/ext/eye_gaze_interaction", &profile));
		OC_XR_LOG(xrStringToPath(xr_instance, "/user/eyes_ext/input/gaze_ext/pose", &pose));
		XrActionSuggestedBinding binding = { eyeGazeAction, pose };
		XrInteractionProfileSuggestedBinding suggested = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
		suggested.interactionProfile = profile;
		suggested.suggestedBindings = &binding;
		suggested.countSuggestedBindings = 1;
		OC_XR_LOG(xr_suggest_bindings_logged(suggested));
	}

	trackerPoseSession = XR_NULL_HANDLE;
	trackerPoseAction = CreateTrackerPoseAction(legacyInputsSet);
	trackerPoseActionGeneration++;
}

// The evaluators' clock: seconds, for the modes' delays and the actions' change times
static double EvaluatorClock()
{
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// A raw input's OpenXR component without its hand: /user/hand/right/input/trigger/click -> /input/trigger/click
static std::string WithoutHand(const std::string& path)
{
	size_t input = path.find("/input/");
	return input == std::string::npos ? path : path.substr(input);
}

void BaseInput::CreateRawActions()
{
	XrActionSetCreateInfo setInfo = { XR_TYPE_ACTION_SET_CREATE_INFO };
	strcpy_arr(setInfo.actionSetName, "openlamina-raw-inputs");
	strcpy_arr(setInfo.localizedActionSetName, "OpenLamina Raw Inputs");
	setInfo.priority = 0;
	OC_XR_LOG(xrCreateActionSet(xr_instance, &setInfo, &rawInputsSet));
	if (rawInputsSet == XR_NULL_HANDLE)
		return;

	// One float action per component any profile has (OpenXR reads a boolean input as 0 or 1 through it, 11.4), on
	// every hand
	for (const std::unique_ptr<InteractionProfile>& profile : InteractionProfile::GetProfileList()) {
		if (!profile->CanHaveBindings())
			continue;
		for (const auto& [xrPath, steamvrPath] : profile->RawComponents()) {
			std::string component = WithoutHand(xrPath);
			if (rawActions.count(component))
				continue;
			std::string name = "raw" + component; // /input/trigger/click -> raw-input-trigger-click
			std::replace(name.begin(), name.end(), '/', '-');
			XrActionCreateInfo info = { XR_TYPE_ACTION_CREATE_INFO };
			info.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
			strcpy_arr(info.actionName, name.c_str());
			strcpy_arr(info.localizedActionName, name.c_str());
			info.subactionPaths = allSubactionPaths.data();
			info.countSubactionPaths = allSubactionPaths.size();
			XrAction action = XR_NULL_HANDLE;
			OC_XR_LOG(xrCreateAction(rawInputsSet, &info, &action));
			if (action != XR_NULL_HANDLE)
				rawActions[component] = action;
		}
	}
}

void BaseInput::SuggestRawInputs(const InteractionProfile& profile, std::vector<XrActionSuggestedBinding>& bindings)
{
	if (rawInputsSet == XR_NULL_HANDLE)
		return;

	std::vector<std::pair<std::string, std::string>>& components = rawComponents[&profile];
	components = profile.RawComponents();
	std::set<std::string> suggested; // a component feeding two SteamVR inputs is suggested once
	for (const auto& [xrPath, steamvrPath] : components) {
		auto action = rawActions.find(WithoutHand(xrPath));
		if (action == rawActions.end() || !suggested.insert(xrPath).second)
			continue;
		XrPath path = XR_NULL_PATH;
		OC_XR_LOG(xrStringToPath(xr_instance, xrPath.c_str(), &path));
		AddSuggestedBinding(bindings, action->second, path);
	}
}

bool BaseInput::SuggestRawBindings(const InteractionProfile& profile, const std::string& bindingsPath, std::vector<XrActionSuggestedBinding>& bindings)
{
	// The binding file chosen for this controller; turned into one for this controller if it's another's. Paths on
	// the primary and secondary hand go to the dominant hand and the other one, again when it changes.
	const BindingFile loaded = LoadBindingFile(PathFromUtf8(bindingsPath));
	const std::string type = profile.BindingConversionType();
	if (!loaded.controllerType.empty() && lowerStr(loaded.controllerType) != type)
		OOVR_LOGF("Binding file: %s's bindings converted for %s", loaded.controllerType.c_str(), type.c_str());
	BindingFile file = BindingFileForController(loaded, type, allSubactionPathNames[DominantHandIndex(HandAlias::PRIMARY)]);
	std::optional<AliasedBindingFile> aliased;
	if (HasHandAliases(loaded))
		aliased = AliasedBindingFile{ loaded, type };

	// Whether the file binds an input this controller has to an action of the manifest, or a haptic was suggested
	auto bindsSomething = [&] {
		if (!bindings.empty())
			return true;
		std::vector<std::string> rawPaths;
		for (const auto& [xrPath, steamvrPath] : profile.RawComponents())
			rawPaths.push_back(steamvrPath);
		return BindsRawInput(file, rawPaths, [&](const std::string& setPath, const std::string& output) {
			const Action* action = actions.LookupItem(actions.ShortenOrLookupName(output));
			return actionSets.LookupItem(setPath) && action && action->type != ActionType::EyeTracking;
		});
	};
	if (!bindsSomething()) {
		aliasedBindingFiles.erase(&profile);
		return false;
	}
	bindingFiles[&profile] = file;
	if (aliased)
		aliasedBindingFiles[&profile] = std::move(*aliased);
	else
		aliasedBindingFiles.erase(&profile);

	if (rawInputsSet == XR_NULL_HANDLE)
		return true;
	SuggestRawInputs(profile, bindings);
	auto evaluator = std::make_unique<BindingEvaluator>(file);
	for (const std::string& warning : evaluator->Warnings())
		OOVR_LOGF("Binding evaluator (%s): %s", profile.GetPath().c_str(), warning.c_str());
	evaluators[&profile] = std::move(evaluator);
	return true;
}

std::optional<std::vector<ActionBinding>> BaseInput::BindingsFromFiles(const Action& action, bool allTypes)
{
	// The files bind inputs, so a pose, a skeleton and a vibration are answered only when allTypes
	if (!allTypes && (action.type == ActionType::Pose || action.type == ActionType::Skeleton || action.type == ActionType::Vibration))
		return std::nullopt;
	std::lock_guard<std::recursive_mutex> evalGuard(evaluatorLock);
	std::optional<std::vector<ActionBinding>> found;
	for (size_t i = 0; i < allSubactionPathNames.size() && i < handInteractionProfiles.size(); i++) {
		// Before the first UpdateActionState the hand's profile is the backend's to tell
		const InteractionProfile* profile = handInteractionProfiles[i];
		if (!profile) {
			auto device = BackendManager::Instance().GetBackendInstance()->GetDeviceByHand(static_cast<ITrackedDevice::TrackedDeviceType>(i));
			profile = device ? device->GetInteractionProfile() : nullptr;
		}
		auto file = bindingFiles.find(profile);
		if (file == bindingFiles.end())
			continue;
		if (!found)
			found.emplace();
		for (ActionBinding& b : BindingsForAction(file->second, action.manifestName, allSubactionPathNames[i]))
			found->push_back(std::move(b));
	}
	return found;
}

std::vector<BaseInput::ActiveSetOnHands> BaseInput::ResolveActiveSets(const VRActiveActionSet_t* sets, uint32_t count)
{
	std::vector<ActiveSetOnHands> resolved;
	for (uint32_t i = 0; i < count; i++) {
		int restrictedHand = -1;
		if (sets[i].ulRestrictedToDevice != vr::k_ulInvalidInputValueHandle) {
			const InputValueHandle* ivh = cast_IVH(sets[i].ulRestrictedToDevice);
			restrictedHand = 2;
			if (ivh && ivh->type == InputSource::HAND_LEFT)
				restrictedHand = 0;
			else if (ivh && ivh->type == InputSource::HAND_RIGHT)
				restrictedHand = 1;
		}
		const SetHands onHands = HandsForActiveSet(restrictedHand);
		ActionSet* primary = cast_ASH(sets[i].ulActionSet);
		ActionSet* secondary = restrictedHand >= 0 ? cast_ASH(sets[i].ulSecondaryActionSet) : nullptr;
		for (auto [set, hands] : { std::pair<ActionSet*, const bool*>{ primary, onHands.primary }, { secondary, onHands.secondary } }) {
			if (!set || !(hands[0] || hands[1]))
				continue;
			ActiveSetOnHands on;
			on.set = set;
			on.priority = sets[i].nPriority;
			on.hands[0] = hands[0];
			on.hands[1] = hands[1];
			resolved.push_back(on);
		}
	}
	return resolved;
}

void BaseInput::UpdateEvaluators(const VRActiveActionSet_t* sets, uint32_t count)
{
	std::lock_guard<std::recursive_mutex> evalGuard(evaluatorLock);
	evaluatedThisSync.clear();
	if (rawInputsSet == XR_NULL_HANDLE)
		return;

	// Each active set with the hand it's restricted to (ulRestrictedToDevice)
	std::vector<BindingEvaluator::ActiveSet> active;
	for (const ActiveSetOnHands& on : ResolveActiveSets(sets, count)) {
		std::string restrictTo;
		if (!(on.hands[0] && on.hands[1]))
			restrictTo = allSubactionPathNames[on.hands[0] ? 0 : 1];
		active.push_back({ on.set->manifestName, on.priority, restrictTo });
	}

	// Each profile a hand uses: its raw inputs from those hands, then its evaluator
	double now = EvaluatorClock();
	std::map<const InteractionProfile*, RawInputState> states;
	for (size_t i = 0; i < allSubactionPaths.size() && i < handInteractionProfiles.size(); i++) {
		const InteractionProfile* profile = handInteractionProfiles[i];
		if (!profile) {
			// The backend learns of the controller from an event the next frame; OpenXR already has it after this sync
			XrInteractionProfileState current = { XR_TYPE_INTERACTION_PROFILE_STATE };
			char name[XR_MAX_PATH_LENGTH] = {};
			uint32_t length = 0;
			if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(xr_session.get(), allSubactionPaths[i], &current))
			    && current.interactionProfile != XR_NULL_PATH
			    && XR_SUCCEEDED(xrPathToString(xr_instance, current.interactionProfile, sizeof(name), &length, name)))
				profile = InteractionProfile::GetProfileByPath(name);
		}
		if (!profile || !evaluators.count(profile))
			continue;
		RawInputState& state = states[profile];
		state.time = now;
		ReadRawInputs(i, *profile, state);
	}
	for (auto& [profile, state] : states) {
		BindingEvaluator& evaluator = *evaluators[profile];
		evaluator.SetActiveActionSets(active);
		evaluator.Update(state);
		evaluatedThisSync.push_back(&evaluator);
	}
}

void BaseInput::ReadRawInputs(size_t hand, const InteractionProfile& profile, RawInputState& state)
{
	const std::string prefix = allSubactionPathNames[hand] + "/";
	auto components = rawComponents.find(&profile);
	if (components == rawComponents.end())
		components = rawComponents.emplace(&profile, profile.RawComponents()).first;
	for (const auto& [xrPath, steamvrPath] : components->second) {
		if (!xrPath.starts_with(prefix))
			continue;
		auto action = rawActions.find(WithoutHand(xrPath));
		if (action == rawActions.end())
			continue;
		XrActionStateGetInfo getInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
		getInfo.action = action->second;
		getInfo.subactionPath = allSubactionPaths[hand];
		XrActionStateFloat value = { XR_TYPE_ACTION_STATE_FLOAT };
		if (XR_SUCCEEDED(xrGetActionStateFloat(xr_session.get(), &getInfo, &value)) && value.isActive)
			state.value[steamvrPath] = value.currentState;
	}
}

const BindingEvaluator* BaseInput::LegacyEvaluatorFor(const InteractionProfile* profile)
{
	if (!profile || !profile->CanHaveBindings() || rawInputsSet == XR_NULL_HANDLE)
		return nullptr;
	std::string type = profile->GetOpenVRName().value_or("generic");
	LegacyEvaluator& legacy = legacyEvaluators[profile];
	if (!legacy.evaluator) {
		const BindingFile* file = LoadLegacyBindings(type);
		if (!file)
			return nullptr;
		legacy.evaluator = std::make_unique<BindingEvaluator>(*file);
		legacy.evaluator->SetActiveActionSets({ { kLegacyActionSet, 0, "" } });
		for (const std::string& warning : legacy.evaluator->Warnings())
			OOVR_LOGF("Legacy binding evaluator (%s): %s", type.c_str(), warning.c_str());
	}

	// Once per sync: the raw inputs of every hand on this profile
	if (legacy.evaluatedAt != syncSerial || !legacy.evaluated) {
		RawInputState state;
		state.time = EvaluatorClock();
		for (size_t i = 0; i < allSubactionPaths.size(); i++) {
			std::shared_ptr<ITrackedDevice> dev = BackendManager::Instance().GetBackendInstance()->GetDeviceByHand((ITrackedDevice::TrackedDeviceType)i);
			if (dev && dev->GetInteractionProfile() == profile)
				ReadRawInputs(i, *profile, state);
		}
		legacy.evaluator->Update(state);
		legacy.evaluatedAt = syncSerial;
		legacy.evaluated = true;
	}
	return legacy.evaluator.get();
}

// The outputs a read restricted to restrictToDevice sees: every binding's if it names no device, else those of the
// bindings on its hand (null if none)
static const ActionOutputs* OutputsOnDevice(const BindingEvaluator& ev, VRInputValueHandle_t restrictToDevice, const std::vector<std::string>& handNames,
    const std::vector<XrPath>& handPaths, bool (*check)(VRInputValueHandle_t, XrPath))
{
	if (restrictToDevice == vr::k_ulInvalidInputValueHandle)
		return &ev.Outputs();
	for (size_t i = 0; i < handNames.size() && i < handPaths.size(); i++) {
		if (check(restrictToDevice, handPaths[i]))
			return ev.OutputsOn(handNames[i]);
	}
	return nullptr;
}

std::optional<DigitalOut> BaseInput::EvaluatedDigital(const Action& action, VRInputValueHandle_t restrictToDevice) const
{
	std::lock_guard<std::recursive_mutex> evalGuard(evaluatorLock);
	// Several controllers' bindings writing one action: the first that's on, else the first that changed (a release on
	// one controller), else the first
	const DigitalOut* found = nullptr;
	for (const BindingEvaluator* ev : evaluatedThisSync) {
		const ActionOutputs* outs = OutputsOnDevice(*ev, restrictToDevice, allSubactionPathNames, allSubactionPaths, &BaseInput::checkRestrictToDevice);
		if (!outs)
			continue;
		auto it = outs->digital.find(action.manifestName);
		if (it == outs->digital.end())
			continue;
		const DigitalOut& d = it->second;
		if (!found || (d.state && !found->state) || (!d.state && !found->state && d.changed && !found->changed))
			found = &d;
	}
	return found ? std::optional<DigitalOut>(*found) : std::nullopt;
}

std::optional<AnalogOut> BaseInput::EvaluatedAnalog(const Action& action, VRInputValueHandle_t restrictToDevice) const
{
	std::lock_guard<std::recursive_mutex> evalGuard(evaluatorLock);
	// Several controllers' bindings writing one action: the largest
	const AnalogOut* found = nullptr;
	float best = -1;
	for (const BindingEvaluator* ev : evaluatedThisSync) {
		const ActionOutputs* outs = OutputsOnDevice(*ev, restrictToDevice, allSubactionPathNames, allSubactionPaths, &BaseInput::checkRestrictToDevice);
		if (!outs)
			continue;
		auto it = outs->analog.find(action.manifestName);
		if (it == outs->analog.end())
			continue;
		const AnalogOut& a = it->second;
		float size = a.x * a.x + a.y * a.y + a.z * a.z;
		if (size > best) {
			best = size;
			found = &a;
		}
	}
	return found ? std::optional<AnalogOut>(*found) : std::nullopt;
}

VRInputValueHandle_t BaseInput::OriginHandle(const std::string& sourcePath)
{
	if (sourcePath.empty())
		return vr::k_ulInvalidInputValueHandle;
	VRInputValueHandle_t handle = vr::k_ulInvalidInputValueHandle;
	GetInputSourceHandle(sourcePath.c_str(), &handle);
	return handle;
}

EVRInputError BaseInput::GetActionSetHandle(const char* pchActionSetName, VRActionSetHandle_t* pHandle)
{
	if (!pchActionSetName || !pHandle)
		return VRInputError_InvalidParam;
	*pHandle = actionSets.LookupHandle(pchActionSetName);
	return vr::VRInputError_None;
}

EVRInputError BaseInput::GetActionHandle(const char* pchActionName, VRActionHandle_t* pHandle)
{
	if (!pchActionName || !pHandle)
		return VRInputError_InvalidParam;
	*pHandle = actions.LookupHandle(pchActionName);
	return vr::VRInputError_None;
}

int BaseInput::DominantHandIndex(HandAlias alias) const
{
	const bool primaryIsLeft = dominantHand == vr::TrackedControllerRole_LeftHand;
	return (alias == HandAlias::PRIMARY) == primaryIsLeft ? 0 : 1;
}

void BaseInput::PointHandleAtHand(InputValueHandle& handle, int hand)
{
	handle.type = hand == 0 ? InputSource::HAND_LEFT : InputSource::HAND_RIGHT;
	handle.devicePathString = hand == 0 ? "/user/hand/left" : "/user/hand/right";
	XrResult res = xr_instance ? xrStringToPath(xr_instance, handle.devicePathString.c_str(), &handle.devicePath) : XR_ERROR_HANDLE_INVALID;
	if (XR_FAILED(res))
		handle.devicePath = XR_NULL_PATH;
}

void BaseInput::ResolveHandAliases()
{
	std::lock_guard<std::mutex> registryGuard(inputHandleRegistryLock);
	for (auto& [path, handle] : inputHandleRegistry) {
		if (handle->alias != HandAlias::NONE)
			PointHandleAtHand(*handle, DominantHandIndex(handle->alias));
	}
}

void BaseInput::ResolveBindingFileHands()
{
	const std::string& primary = allSubactionPathNames[DominantHandIndex(HandAlias::PRIMARY)];
	// The evaluators replaced here may be among the last sync's: none is read again until the next one
	evaluatedThisSync.clear();
	for (const auto& [profile, aliased] : aliasedBindingFiles) {
		auto file = bindingFiles.find(profile);
		if (file == bindingFiles.end())
			continue;
		file->second = BindingFileForController(aliased.file, aliased.controllerType, primary);
		if (evaluators.count(profile))
			evaluators[profile] = std::make_unique<BindingEvaluator>(file->second);
	}
}

EVRInputError BaseInput::GetInputSourceHandle(const char* pchInputSourcePath, VRInputValueHandle_t* pHandle)
{
	// Get the existing InputValueHandle if it already exists, or make a new one otherwise. Applications can
	// get whatever handles they want, regardless of whether the runtime associates any special meaning with it.

	if (!pchInputSourcePath || !pHandle)
		return VRInputError_InvalidParam;

	std::lock_guard<std::mutex> registryGuard(inputHandleRegistryLock);
	const auto iter = inputHandleRegistry.find(pchInputSourcePath);
	if (iter != inputHandleRegistry.end()) {
		InputValueHandle* handle = iter->second.get();
		*pHandle = (uint64_t)handle;
		return VRInputError_None;
	}

	std::unique_ptr<InputValueHandle> handle = std::make_unique<InputValueHandle>();
	InputValueHandle* ptr = handle.get();
	handle->path = pchInputSourcePath;

	const InputSourceInfo info = ClassifyInputSource(pchInputSourcePath);
	handle->type = info.type;
	handle->alias = info.alias;
	if (info.alias != HandAlias::NONE) {
		PointHandleAtHand(*handle, DominantHandIndex(info.alias));
	} else if (!info.devicePath.empty()) {
		handle->devicePathString = info.devicePath;
		// Without an OpenXR instance (a Utility app) the handle still works; it just matches no subaction path
		XrResult res = xr_instance ? xrStringToPath(xr_instance, handle->devicePathString.c_str(), &handle->devicePath) : XR_ERROR_HANDLE_INVALID;
		if (XR_FAILED(res)) {
			OOVR_LOGF("GetInputSourceHandle: no OpenXR path for %s (%d)", handle->devicePathString.c_str(), (int)res);
			handle->devicePath = XR_NULL_PATH;
		}
	}

	inputHandleRegistry[pchInputSourcePath] = std::move(handle);
	*(uint64_t*)pHandle = (uint64_t)ptr;
	return VRInputError_None;
}

EVRInputError BaseInput::UpdateActionState(VR_ARRAY_COUNT(unSetCount) VRActiveActionSet_t* pSets,
    uint32_t unSizeOfVRSelectedActionSet_t, uint32_t unSetCount)
{
	auto sessionLock = xr_session.lock_shared(); // held while reading: a session restart waits for it
	// TODO if the game is using legacy input, call this every frame

	if (EVRInputError err = CheckActiveSets(pSets, unSizeOfVRSelectedActionSet_t, unSetCount); err != VRInputError_None)
		return err;

	{
		// The aliases and the hands' profiles are read by other threads' calls under the evaluator lock
		std::lock_guard<std::recursive_mutex> evalGuard(evaluatorLock);
		if (dominantHand != requestedDominantHand.load()) {
			dominantHand = requestedDominantHand.load();
			ResolveHandAliases();
			ResolveBindingFileHands();
		}
	}

	for (uint32_t i = 0; i < unSetCount; i++) {
		if (!cast_ASH(pSets[i].ulActionSet)) {
			OOVR_LOG_ONCE("WARNING: Invalid action set handle passed!");
			return VRInputError_InvalidHandle;
		}
	}

	// A set on both hands (named twice, once for each, or by a secondary set) is one entry for every hand
	std::vector<XrActiveActionSet> aas;
	std::vector<std::array<bool, 2>> onHands;
	for (const ActiveSetOnHands& on : ResolveActiveSets(pSets, unSetCount)) {
		auto entry = std::find_if(aas.begin(), aas.end(), [&](const XrActiveActionSet& a) { return a.actionSet == on.set->xr; });
		if (entry == aas.end()) {
			aas.push_back({ on.set->xr, XR_NULL_PATH });
			onHands.push_back({ false, false });
			entry = aas.end() - 1;
		}
		std::array<bool, 2>& hands = onHands[entry - aas.begin()];
		hands[0] = hands[0] || on.hands[0];
		hands[1] = hands[1] || on.hands[1];
	}
	for (size_t i = 0; i < aas.size(); i++) {
		if (!(onHands[i][0] && onHands[i][1]))
			aas[i].subactionPath = allSubactionPaths.at(onHands[i][0] ? 0 : 1);
	}

	// Ad the last set, the legacy input set; then the raw inputs the evaluator reads
	aas.push_back({ legacyInputsSet, XR_NULL_PATH });
	if (rawInputsSet != XR_NULL_HANDLE)
		aas.push_back({ rawInputsSet, XR_NULL_PATH });

	// A set OpenXR refused to create is left out, or the whole sync would fail
	aas.erase(std::remove_if(aas.begin(), aas.end(), [](const XrActiveActionSet& a) { return a.actionSet == XR_NULL_HANDLE; }), aas.end());

	XrActionsSyncInfo syncInfo = { XR_TYPE_ACTIONS_SYNC_INFO };
	syncInfo.activeActionSets = aas.data();
	syncInfo.countActiveActionSets = aas.size();
	OC_XR_LOG(xrSyncActions(xr_session.get(), &syncInfo));
	syncSerial++;
	FlushDueHaptics();

	std::lock_guard<std::recursive_mutex> evalGuard(evaluatorLock);
	for (size_t i = 0; i < allSubactionPaths.size(); i++) {
		auto handDevice = BackendManager::Instance().GetBackendInstance()->GetDeviceByHand(static_cast<ITrackedDevice::TrackedDeviceType>(i));
		auto newProfile = handDevice == nullptr ? nullptr : handDevice->GetInteractionProfile();
		if (handInteractionProfiles[i] != newProfile) {
			OOVR_LOGF("Hand %s switching to profile %s", allSubactionPathNames[i].c_str(), newProfile == nullptr ? "<null>" : newProfile->GetPath().c_str());
			handInteractionProfiles[i] = newProfile;
		}
	}

	UpdateEvaluators(pSets, unSetCount);

	return VRInputError_None;
}

void BaseInput::InternalUpdate()
{
	auto sessionLock = xr_session.lock_shared(); // held while reading: a session restart waits for it
	if (!usingLegacyInput)
		return;

	XrActiveActionSet aas[2] = { { legacyInputsSet }, { rawInputsSet } };

	XrActionsSyncInfo syncInfo = { XR_TYPE_ACTIONS_SYNC_INFO };
	syncInfo.activeActionSets = aas;
	syncInfo.countActiveActionSets = rawInputsSet != XR_NULL_HANDLE ? 2 : 1;
	OC_XR_LOG(xrSyncActions(xr_session.get(), &syncInfo));
	syncSerial++;
	FlushDueHaptics();
}

EVRInputError BaseInput::GetDigitalActionData(VRActionHandle_t action, InputDigitalActionData_t* pActionData, uint32_t unActionDataSize,
    VRInputValueHandle_t ulRestrictToDevice)
{
	auto sessionLock = xr_session.lock_shared(); // held while reading: a session restart waits for it
	// Zeroed before the handle is looked up: an action left out of the manifest answers InvalidHandle, and inactive
	if (pActionData && unActionDataSize == sizeof(*pActionData))
		ZeroMemory(pActionData, unActionDataSize);
	GET_ACTION_FROM_HANDLE(act, action);

	// A null or wrong-sized struct is an argument error, as SteamVR answers (VRInputError_InvalidParam)
	if (!pActionData || unActionDataSize != sizeof(*pActionData))
		return VRInputError_InvalidParam;

	if (EVRInputError err = DigitalReadError(act->type); err != VRInputError_None)
		return err;

	// Held by the dashboard or the grid: inactive (zeroed above)
	if (InputHeld())
		return VRInputError_None;

	// The evaluators' output, inactive if no active binding writes it
	if (rawInputsSet != XR_NULL_HANDLE) {
		if (std::optional<DigitalOut> d = EvaluatedDigital(*act, ulRestrictToDevice)) {
			pActionData->bActive = true;
			pActionData->bState = d->state;
			pActionData->bChanged = d->changed;
			pActionData->fUpdateTime = UpdateTimeFromNow(d->changeTime, act->createdAt, EvaluatorClock());
			pActionData->activeOrigin = OriginHandle(d->origin);
		}
	}
	return VRInputError_None;
}

EVRInputError BaseInput::GetAnalogActionData(VRActionHandle_t action, InputAnalogActionData_t* pActionData, uint32_t unActionDataSize,
    VRInputValueHandle_t ulRestrictToDevice)
{
	auto sessionLock = xr_session.lock_shared(); // held while reading: a session restart waits for it
	// Zeroed before the handle is looked up: an action left out of the manifest answers InvalidHandle, and inactive
	if (pActionData && unActionDataSize == sizeof(*pActionData))
		ZeroMemory(pActionData, unActionDataSize);
	GET_ACTION_FROM_HANDLE(act, action);
	// A null or wrong-sized struct is an argument error, as SteamVR answers (VRInputError_InvalidParam)
	if (!pActionData || unActionDataSize != sizeof(*pActionData))
		return VRInputError_InvalidParam;

	if (act->type != ActionType::Vector1 && act->type != ActionType::Vector2 && act->type != ActionType::Vector3)
		OC_UNSUPPORTED(VRInputError_WrongType, "Invalid action type %d for action %s",
		    static_cast<int>(act->type), act->fullName.c_str());

	// Held by the dashboard or the grid: inactive (zeroed above)
	if (InputHeld())
		return VRInputError_None;

	// The evaluators' output, inactive if no active binding writes it
	if (rawInputsSet != XR_NULL_HANDLE) {
		if (std::optional<AnalogOut> a = EvaluatedAnalog(*act, ulRestrictToDevice)) {
			pActionData->bActive = true;
			pActionData->x = a->x;
			pActionData->y = act->type == ActionType::Vector1 ? 0 : a->y;
			pActionData->z = act->type == ActionType::Vector3 ? a->z : 0;
			pActionData->deltaX = a->dx;
			pActionData->deltaY = act->type == ActionType::Vector1 ? 0 : a->dy;
			pActionData->deltaZ = act->type == ActionType::Vector3 ? a->dz : 0;
			pActionData->fUpdateTime = UpdateTimeFromNow(a->changeTime, act->createdAt, EvaluatorClock());
			pActionData->activeOrigin = OriginHandle(a->origin);
		}
	}
	return VRInputError_None;
}

void BaseInput::ApplyPoseComponent(const vr::TrackedDevicePose_t& rawPose, ITrackedDevice::TrackedDeviceType hand, const char* componentName, vr::TrackedDevicePose_t& out)
{
	if (!rawPose.bPoseIsValid)
		return;

	glm::mat4 handMat = S2G_m34(rawPose.mDeviceToAbsoluteTracking);

	// Add the component transform, if required
	OOVR_RenderModel_ComponentState_t state = {};
	const bool hasTransform = componentName && GetBaseRenderModels()->TryGetComponentState(hand, componentName, &state);
	if (hasTransform)
		handMat = handMat * S2G_m34(state.mTrackingToComponentLocal);

	// Store the final hand matrix
	out.mDeviceToAbsoluteTracking = G2S_m34(handMat);
	out.bPoseIsValid = true;
	out.bDeviceIsConnected = true;
	out.eTrackingResult = rawPose.eTrackingResult;

	// The velocities stay in the tracking space. The angular one is the same everywhere on the device; the
	// component's point is away from the device's, so its linear one adds the turn's: w x (component - device).
	glm::vec3 velocity = S2G_v3f(rawPose.vVelocity);
	if (hasTransform) {
		const glm::vec3 offset = glm::vec3(handMat[3]) - glm::vec3(S2G_m34(rawPose.mDeviceToAbsoluteTracking)[3]);
		velocity += glm::cross(S2G_v3f(rawPose.vAngularVelocity), offset);
	}
	out.vVelocity = G2S_v3f(velocity);
	out.vAngularVelocity = rawPose.vAngularVelocity;
}

EVRInputError BaseInput::GetPoseActionData(VRActionHandle_t action, ETrackingUniverseOrigin eOrigin, float fPredictedSecondsFromNow,
    InputPoseActionData_t* pActionData, uint32_t unActionDataSize, VRInputValueHandle_t ulRestrictToDevice)
{
	auto sessionLock = xr_session.lock_shared(); // held while reading: a session restart waits for it
	// Zeroed before the handle is looked up: an action left out of the manifest answers InvalidHandle, and inactive
	if (pActionData && unActionDataSize == sizeof(*pActionData))
		ZeroMemory(pActionData, unActionDataSize);
	GET_ACTION_FROM_HANDLE(act, action);

	// A null or wrong-sized struct is an argument error, as SteamVR answers (VRInputError_InvalidParam)
	if (!pActionData || unActionDataSize != sizeof(*pActionData))
		return VRInputError_InvalidParam;

	if (act->type == ActionType::Skeleton) {
		std::shared_ptr<ITrackedDevice> dev = BackendManager::Instance().GetDeviceByHand(act->skeletalHand);
		if (!dev)
			return vr::VRInputError_InvalidDevice;

		// The skeleton's root is at the hand's grip, as a pose bound to the hand's own grip is
		vr::TrackedDevicePose_t rawPose = {};
		dev->GetPose(eOrigin, &rawPose, ETrackingStateType::TrackingStateType_Now);
		const int hand = act->skeletalHand == ITrackedDevice::HAND_LEFT ? 0 : 1;
		pActionData->pose = rawPose;
		ApplyPoseComponent(rawPose, act->skeletalHand, "handgrip", pActionData->pose);
		pActionData->bActive = pActionData->pose.bPoseIsValid && pActionData->pose.bDeviceIsConnected;
		pActionData->activeOrigin = vr::k_ulInvalidInputValueHandle;
		GetInputSourceHandle(allSubactionPathNames[hand].c_str(), &pActionData->activeOrigin);

		return vr::VRInputError_None;
	}

	if (act->type != ActionType::Pose)
		OC_UNSUPPORTED(VRInputError_WrongType, "Invalid action type %d for action %s", static_cast<int>(act->type), act->fullName.c_str());

	const InputValueHandle* restrictToDevice = nullptr;
	if (ulRestrictToDevice) {
		restrictToDevice = cast_IVH(ulRestrictToDevice);
		if (!restrictToDevice)
			return VRInputError_InvalidHandle;
	}

	for (int handNum = 0; handNum < 2; handNum++) {
		// Check this hand is permitted
		ITrackedDevice::TrackedDeviceType handType = handNum == 0 ? ITrackedDevice::HAND_LEFT : ITrackedDevice::HAND_RIGHT;
		InputSource sourceType = handNum == 0 ? InputSource::HAND_LEFT : InputSource::HAND_RIGHT;

		if (restrictToDevice && restrictToDevice->type != sourceType) {
			continue;
		}

		// Find the device for this hand, and look up it's interaction profile
		std::shared_ptr<ITrackedDevice> dev = BackendManager::Instance().GetDeviceByHand(handType);
		if (!dev)
			continue;

		const InteractionProfile* profile = dev->GetInteractionProfile();
		if (!profile)
			continue;

		const PoseBindingInfo* found = nullptr;
		{
			const auto& bindings = handType == ITrackedDevice::HAND_LEFT ? act->poseBindingsLeft : act->poseBindingsRight;
			const auto& byDominance = DominantHandIndex(HandAlias::PRIMARY) == handNum ? act->poseBindingsPrimary : act->poseBindingsSecondary;
			for (const auto* map : { &bindings, &byDominance }) {
				auto it = map->find(profile);
				if (it != map->end()) {
					found = &it->second;
					break;
				}
			}
		}
		if (!found)
			continue;
		const PoseBindingInfo& binding = *found;

		// TODO use predicted time

		// Regardless of whether valid data is available, this action is bound
		pActionData->bActive = true;

		// The hand itself is the origin: a pose is bound to a hand, not to one of its inputs
		pActionData->activeOrigin = vr::k_ulInvalidInputValueHandle;
		GetInputSourceHandle(allSubactionPathNames[handNum].c_str(), &pActionData->activeOrigin);

		vr::TrackedDevicePose_t rawPose = {};
		dev->GetPose(eOrigin, &rawPose, TrackingStateType_Now);

		const char* componentName = nullptr;
		switch (binding.point) {
		case PoseBindingPoint::RAW:
		case PoseBindingPoint::BODY:
		case PoseBindingPoint::GDC2015:
			// No component name
			break;
		case PoseBindingPoint::GRIP:
			componentName = "grip";
			break;
		case PoseBindingPoint::BASE:
			componentName = "base";
			break;
		case PoseBindingPoint::HANDGRIP:
			componentName = "handgrip";
			break;
		case PoseBindingPoint::TIP:
			componentName = "tip";
			break;
		}
		ApplyPoseComponent(rawPose, handType, componentName, pActionData->pose);

		// Stop as soon as we find the first available input (and a disconnected controller is
		// fine to count as available - that's probably not how it should be, but it'd be a
		// pain to do it properly)
		break;
	}

	// Bound on no hand: a tracker, if a tracker's binding file binds it
	if (!pActionData->bActive)
		TrackerPoseData(act->manifestName, eOrigin, restrictToDevice, pActionData);

	return VRInputError_None;
}

bool BaseInput::TrackerPoseData(const std::string& action, vr::ETrackingUniverseOrigin origin, const InputValueHandle* restrictToDevice,
    InputPoseActionData_t* data)
{
	auto routes = trackerPoseRoutes.find(action);
	if (routes == trackerPoseRoutes.end())
		return false;
	for (const std::string& roleName : routes->second) {
		std::optional<ViveTrackerRole> role;
		for (const ViveTrackerRole& r : ViveTrackerRoles())
			if (roleName == r.name)
				role = r;
		if (!role || (restrictToDevice && restrictToDevice->devicePathString != role->steamvrPath))
			continue;
		std::shared_ptr<ITrackedDevice> dev = TrackerForRole(*role);
		if (!dev)
			continue;
		data->bActive = true;
		data->activeOrigin = vr::k_ulInvalidInputValueHandle;
		GetInputSourceHandle(role->steamvrPath, &data->activeOrigin);
		dev->GetPose(origin, &data->pose, TrackingStateType_Now);
		return true;
	}
	return false;
}

std::shared_ptr<ITrackedDevice> BaseInput::TrackerForRole(const ViveTrackerRole& role)
{
	// The tracker with that role: its controller type names it
	const std::string type = ViveTrackerControllerType(role);
	for (vr::TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; i++) {
		std::shared_ptr<ITrackedDevice> dev = BackendManager::Instance().GetDevice(i);
		if (!dev || dev->GetTrackedDeviceClass() != vr::TrackedDeviceClass_GenericTracker)
			continue;
		char buffer[vr::k_unMaxPropertyStringSize] = {};
		vr::ETrackedPropertyError err = vr::TrackedProp_Success;
		dev->GetStringTrackedDeviceProperty(vr::Prop_ControllerType_String, buffer, sizeof(buffer), &err);
		if (err == vr::TrackedProp_Success && type == buffer)
			return dev;
	}
	return nullptr;
}

EVRInputError BaseInput::GetPoseActionDataRelativeToNow(VRActionHandle_t action, ETrackingUniverseOrigin eOrigin, float fPredictedSecondsFromNow, InputPoseActionData_t* pActionData, uint32_t unActionDataSize, VRInputValueHandle_t ulRestrictToDevice)
{
	// Same function, different name - the 'RelativeToNow' suffix was added when GetPoseActionDataForNextFrame was added
	return GetPoseActionData(action, eOrigin, fPredictedSecondsFromNow, pActionData, unActionDataSize, ulRestrictToDevice);
}
EVRInputError BaseInput::GetPoseActionDataForNextFrame(VRActionHandle_t action, ETrackingUniverseOrigin eOrigin, InputPoseActionData_t* pActionData, uint32_t unActionDataSize, VRInputValueHandle_t ulRestrictToDevice)
{
	return GetPoseActionData(action, eOrigin, 0, pActionData, unActionDataSize, ulRestrictToDevice);
}
EVRInputError BaseInput::GetSkeletalActionData(VRActionHandle_t action, InputSkeletalActionData_t* pActionData, uint32_t unActionDataSize,
    VRInputValueHandle_t ulRestrictToDevice)
{
	// This is the old version of the function, the new one doesn't have the device restriction
	// Hopefully noone depends on this and we can just ignore it
	if (ulRestrictToDevice != vr::k_ulInvalidInputValueHandle) {
		OOVR_SOFT_ABORT("Old GetSkeletalActionData device restrictions not supported");
	}

	return GetSkeletalActionData(action, pActionData, unActionDataSize);
}
EVRInputError BaseInput::GetSkeletalActionData(VRActionHandle_t actionHandle, InputSkeletalActionData_t* out, uint32_t unActionDataSize)
{
	auto sessionLock = xr_session.lock_shared(); // held while reading: a session restart waits for it
	// Make sure the target struct is the right size, in case it grows in the future
	// Note: CVRInput_004 has a manual override for this in CVRInput.cpp to deal with an old struct version, it
	// sets unActionDataSize to what we're expecting so we don't have to handle that here.
	// A null or wrong-sized struct is an argument error, as SteamVR answers (VRInputError_InvalidParam)
	if (!out || unActionDataSize != sizeof(*out))
		return VRInputError_InvalidParam;

	// Zero out the output data, leaving:
	// bActive = false
	// activeOrigin = vr::k_ulInvalidActionHandle
	// (before the handle is looked up: an action left out of the manifest answers InvalidHandle, and inactive)
	ZeroMemory(out, unActionDataSize);

	GET_ACTION_FROM_HANDLE(action, actionHandle);

	// Find the active origin for this hand
	std::string originPath;
	if (action->skeletalHand == ITrackedDevice::HAND_LEFT)
		originPath = "/user/hand/left";
	else
		originPath = "/user/hand/right";
	if (GetInputSourceHandle(originPath.c_str(), &out->activeOrigin) != vr::VRInputError_None)
		out->activeOrigin = vr::k_ulInvalidInputValueHandle;

	out->bActive = ivhToDev(out->activeOrigin) != nullptr;
	return vr::VRInputError_None;
}

EVRInputError BaseInput::GetDominantHand(vr::ETrackedControllerRole* peDominantHand)
{
	if (!peDominantHand)
		return VRInputError_InvalidParam;
	// Only an app that says in its manifest it has the setting gets an answer
	if (!allowSetDominantHand)
		return VRInputError_PermissionDenied;
	*peDominantHand = dominantHand;
	return VRInputError_None;
}
EVRInputError BaseInput::SetDominantHand(vr::ETrackedControllerRole eDominantHand)
{
	if (!allowSetDominantHand)
		return VRInputError_PermissionDenied;
	if (eDominantHand != vr::TrackedControllerRole_LeftHand && eDominantHand != vr::TrackedControllerRole_RightHand)
		return VRInputError_InvalidParam;
	// Taken up at the next UpdateActionState
	requestedDominantHand = eDominantHand;
	return VRInputError_None;
}
EVRInputError BaseInput::GetBoneCount(VRActionHandle_t action, uint32_t* pBoneCount)
{
	if (!pBoneCount)
		return VRInputError_InvalidParam;
	GET_ACTION_FROM_HANDLE(act, action);
	if (!IsSkeletonHand(act->skeletalHand))
		return VRInputError_InvalidSkeleton;
	*pBoneCount = (int)eBone_Count;
	return vr::VRInputError_None;
}
EVRInputError BaseInput::GetBoneHierarchy(VRActionHandle_t action, VR_ARRAY_COUNT(unIndexArayCount) BoneIndex_t* pParentIndices, uint32_t unIndexArayCount)
{
	GET_ACTION_FROM_HANDLE(act, action);
	if ((int)act->skeletalHand >= 2)
		return VRInputError_InvalidSkeleton;
	if (!pParentIndices || unIndexArayCount < eBone_Count)
		return VRInputError_BufferTooSmall;
	for (BoneIndex_t i = 0; i < eBone_Count; i++)
		pParentIndices[i] = HandBoneParent(i);
	return VRInputError_None;
}
EVRInputError BaseInput::GetBoneName(VRActionHandle_t action, BoneIndex_t nBoneIndex, VR_OUT_STRING() char* pchBoneName, uint32_t unNameBufferSize)
{
	if (pchBoneName && unNameBufferSize)
		pchBoneName[0] = 0;
	GET_ACTION_FROM_HANDLE(act, action);
	if ((int)act->skeletalHand >= 2)
		return VRInputError_InvalidSkeleton;
	if (nBoneIndex < 0 || nBoneIndex >= eBone_Count)
		return VRInputError_InvalidBoneIndex;
	std::string name = HandBoneName(nBoneIndex, act->skeletalHand == ITrackedDevice::HAND_RIGHT);
	if (!pchBoneName || unNameBufferSize < name.size() + 1)
		return VRInputError_BufferTooSmall;
	memcpy(pchBoneName, name.c_str(), name.size() + 1);
	return VRInputError_None;
}
EVRInputError BaseInput::GetSkeletalReferenceTransforms(VRActionHandle_t actionHandle, EVRSkeletalTransformSpace eTransformSpace, EVRSkeletalReferencePose eReferencePose, VR_ARRAY_COUNT(unTransformArrayCount) VRBoneTransform_t* pTransformArray, uint32_t unTransformArrayCount)
{
	GET_ACTION_FROM_HANDLE(act, actionHandle);

	if (!IsSkeletonHand(act->skeletalHand))
		return VRInputError_InvalidSkeleton;
	if (EVRInputError err = CheckBoneArray(pTransformArray, unTransformArrayCount); err != VRInputError_None)
		return err;

	std::span<VRBoneTransform_t> out(pTransformArray, unTransformArrayCount);

	// The poses don't change with the controller, so a hand with no controller (yet) gets the default ones
	const InteractionProfile* profile = nullptr;
	if (std::shared_ptr<ITrackedDevice> dev = BackendManager::Instance().GetDeviceByHand(act->skeletalHand))
		profile = dev->GetInteractionProfile();

	const std::optional<BoneArray> pose = profile
	    ? profile->GetSkeletalReferencePose(act->skeletalHand, eReferencePose)
	    : InteractionProfile::GetDefaultSkeletalReferencePose(act->skeletalHand, eReferencePose);
	if (!pose.has_value()) {
		OOVR_LOGF("WARNING: Couldn't find reference pose: %d, %d", act->skeletalHand, eReferencePose);
		return vr::VRInputError_InvalidParam;
	}

	std::copy(std::begin(pose.value()), std::end(pose.value()), out.begin());

	if (eTransformSpace == EVRSkeletalTransformSpace::VRSkeletalTransformSpace_Model)
		ParentSpaceSkeletonToModelSpace(pTransformArray);

	return vr::VRInputError_None;
}
EVRInputError BaseInput::GetSkeletalTrackingLevel(VRActionHandle_t action, EVRSkeletalTrackingLevel* pSkeletalTrackingLevel)
{
	auto sessionLock = xr_session.lock_shared(); // held while reading: a session restart waits for it
	GET_ACTION_FROM_HANDLE(act, action);

	if (act->skeletalHand == ITrackedDevice::HAND_NONE)
		return vr::VRInputError_InvalidHandle;

	std::shared_ptr<ITrackedDevice> dev = BackendManager::Instance().GetDeviceByHand(act->skeletalHand);
	if (!dev)
		return vr::VRInputError_InvalidDevice;

	const InteractionProfile* profile = dev->GetInteractionProfile();

	// HACK until we have hand tracking data source in monado, i'm hardcoding this here so games won't think the index is a proper hand.
	if (profile && profile->GetPath() == "/interaction_profiles/valve/index_controller") {
		*pSkeletalTrackingLevel = vr::VRSkeletalTracking_Partial;
		return vr::VRInputError_None;
	}

	if (dev->IsHandTrackingValid()) {
		*pSkeletalTrackingLevel = vr::VRSkeletalTracking_Full;
		return vr::VRInputError_None;
	}

	if (profile) {
		std::optional<vr::EVRSkeletalTrackingLevel> level = profile->GetOpenVRTrackinglevel();
		if (level.has_value()) {
			*pSkeletalTrackingLevel = static_cast<vr::EVRSkeletalTrackingLevel>(level.value());
			return vr::VRInputError_None;
		} else {
			*pSkeletalTrackingLevel = vr::VRSkeletalTracking_Estimated;
		}
	} else {
		*pSkeletalTrackingLevel = vr::VRSkeletalTracking_Estimated;
	}

	return vr::VRInputError_None;
}
EVRInputError BaseInput::GetSkeletalBoneData(VRActionHandle_t action, EVRSkeletalTransformSpace eTransformSpace,
    EVRSkeletalMotionRange eMotionRange, VR_ARRAY_COUNT(unTransformArrayCount) VRBoneTransform_t* pTransformArray,
    uint32_t unTransformArrayCount, VRInputValueHandle_t ulRestrictToDevice)
{
	// This is the old version of the function, the new one doesn't have the device restriction
	// Hopefully noone depends on this and we can just ignore it
	if (ulRestrictToDevice != vr::k_ulInvalidInputValueHandle) {
		OOVR_SOFT_ABORT("Old skeletal input device restrictions not supported");
	}

	return GetSkeletalBoneData(action, eTransformSpace, eMotionRange, pTransformArray, unTransformArrayCount);
}

EVRInputError BaseInput::GetSkeletalBoneData(VRActionHandle_t actionHandle, EVRSkeletalTransformSpace eTransformSpace,
    EVRSkeletalMotionRange eMotionRange, VR_ARRAY_COUNT(unTransformArrayCount) VRBoneTransform_t* pTransformArray, uint32_t unTransformArrayCount)
{
	auto sessionLock = xr_session.lock_shared(); // held while reading: a session restart waits for it
	ClearBones(pTransformArray, unTransformArrayCount);
	GET_ACTION_FROM_HANDLE(action, actionHandle);

	// FIXME remove
	// Hand labs is in parent transform space, and usually 'with controller' motion range
	// OOVR_LOGF("skeletal data %d %d", eTransformSpace, eMotionRange);

	// Check for the right number of bones (and a hand), as SteamVR answers a wrong count
	if (!pTransformArray || unTransformArrayCount != 31)
		return vr::VRInputError_InvalidParam;

	const auto hand = action->skeletalHand;
	if (static_cast<int>(hand) >= 2)
		return vr::VRInputError_InvalidSkeleton;

	std::shared_ptr<ITrackedDevice> dev = BackendManager::Instance().GetDeviceByHand(hand);
	if (!dev)
		return vr::VRInputError_InvalidDevice;

	if (!xr_gbl || !xr_gbl->handTrackingProperties.supportsHandTracking) {
		dev->SetHandTrackingValid(false);
		return getEstimatedBoneData(hand, eTransformSpace, eMotionRange, std::span<VRBoneTransform_t, eBone_Count>(pTransformArray, eBone_Count));
	}

	bool usingFakePose = dev->IsPoseFromHandTracking();

	XrHandJointsLocateInfoEXT locateInfo = { XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT };
	// If the pose is fake that means we don't have controllers the hand must be located relative to floor
	locateInfo.baseSpace = usingFakePose ? xr_gbl->floorSpace : legacyControllers[static_cast<int>(hand)].gripPoseSpace;
	locateInfo.time = xr_gbl->GetBestTime();

	XrHandJointLocationsEXT locations = { XR_TYPE_HAND_JOINT_LOCATIONS_EXT };
	locations.jointCount = XR_HAND_JOINT_COUNT_EXT;
	std::vector<XrHandJointLocationEXT> jointLocations(locations.jointCount);
	locations.jointLocations = jointLocations.data();

	if (handTrackers[(int)hand] == XR_NULL_HANDLE) {
		dev->SetHandTrackingValid(false);
		return getEstimatedBoneData(hand, eTransformSpace, eMotionRange, std::span<VRBoneTransform_t, eBone_Count>(pTransformArray, eBone_Count));
	}

	OC_XR_LOG(xr_ext->xrLocateHandJointsEXT(handTrackers[(int)hand], &locateInfo, &locations));

	if (!locations.isActive) {
		// Fallback to estimated bone data (e.g. for controllers)
		dev->SetHandTrackingValid(false);
		return getEstimatedBoneData(hand, eTransformSpace, eMotionRange, std::span<VRBoneTransform_t, eBone_Count>(pTransformArray, eBone_Count));
	}

	bool isRight = (action->skeletalHand == ITrackedDevice::HAND_RIGHT);
	const InteractionProfile* profile = dev->GetInteractionProfile();

	// Retrieve the appropriate hand transform to account for the SteamVR pose being different from OpenXR grip
	glm::mat4 transform = profile->GetGripToSteamVRTransform(hand);

	if (!XrHandJointsToSkeleton(jointLocations, isRight, pTransformArray, transform))
		return getEstimatedBoneData(hand, eTransformSpace, eMotionRange, std::span<VRBoneTransform_t, eBone_Count>(pTransformArray, eBone_Count));

	// Without a controller we get a hand positioned relative to floor space, change that to be relative to the fake controller pose
	if (usingFakePose) {
		vr::VRBoneTransform_t& wrist = pTransformArray[eBone_Wrist];
		if (isRight) {
			wrist.position = { 0.034038f, 0.036503f, 0.164722f, 1.000000f };
			wrist.orientation = { -0.055147f, -0.078608f, 0.920279f, -0.379296f };
		} else {
			wrist.position = { -0.034038f, 0.036503f, 0.164722f, 1.000000f };
			wrist.orientation = { -0.055147f, -0.078608f, -0.920279f, 0.379296f };
		}
	}

	if (eTransformSpace == EVRSkeletalTransformSpace::VRSkeletalTransformSpace_Model)
		ParentSpaceSkeletonToModelSpace(pTransformArray);

	dev->SetHandTrackingValid(true);

	return vr::VRInputError_None;
}
EVRInputError BaseInput::GetSkeletalSummaryData(VRActionHandle_t actionHandle, EVRSummaryType eSummaryType, VRSkeletalSummaryData_t* pSkeletalSummaryData)
{
	auto sessionLock = xr_session.lock_shared(); // held while reading: a session restart waits for it
	GET_ACTION_FROM_HANDLE(action, actionHandle);

	if (!pSkeletalSummaryData)
		return VRInputError_InvalidParam;
	ZeroMemory(pSkeletalSummaryData, sizeof(VRSkeletalSummaryData_t));

	if (!IsSkeletonHand(action->skeletalHand))
		return VRInputError_InvalidSkeleton;

	if (xr_gbl && xr_gbl->handTrackingProperties.supportsHandTracking) {
		return getRealSkeletalSummary(action->skeletalHand, pSkeletalSummaryData);
	}

	return getEstimatedSkeletalSummary(action->skeletalHand, pSkeletalSummaryData);
}

EVRInputError BaseInput::getRealSkeletalSummary(ITrackedDevice::TrackedDeviceType hand, VRSkeletalSummaryData_t* pSkeletalSummaryData)
{
	auto sessionLock = xr_session.lock_shared(); // held while reading: a session restart waits for it
	if (!xr_gbl || !IsSkeletonHand(hand) || handTrackers[hand] == XR_NULL_HANDLE)
		return getEstimatedSkeletalSummary(hand, pSkeletalSummaryData);
	XrHandJointsLocateInfoEXT locateInfo = { XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT };
	locateInfo.baseSpace = xr_gbl->floorSpace;
	locateInfo.time = xr_gbl->GetBestTime();

	XrHandJointLocationsEXT locations = { XR_TYPE_HAND_JOINT_LOCATIONS_EXT };
	locations.jointCount = XR_HAND_JOINT_COUNT_EXT;
	std::vector<XrHandJointLocationEXT> jointLocations(locations.jointCount);
	locations.jointLocations = jointLocations.data();

	OC_XR_LOG(xr_ext->xrLocateHandJointsEXT(handTrackers[hand], &locateInfo, &locations));

	if (!locations.isActive) {
		// Fallback to estimated skeletal summary data (e.g. for controllers)
		return getEstimatedSkeletalSummary(hand, pSkeletalSummaryData);
	}

	for (int i = 0; i < 5; ++i) {
		XrHandJointLocationEXT metacarpal, proximal, tip;

		switch (i) {
		case 0: // thumb
			metacarpal = jointLocations[XR_HAND_JOINT_THUMB_METACARPAL_EXT];
			proximal = jointLocations[XR_HAND_JOINT_THUMB_PROXIMAL_EXT];
			tip = jointLocations[XR_HAND_JOINT_THUMB_TIP_EXT];
			break;
		case 1: // index
			metacarpal = jointLocations[XR_HAND_JOINT_INDEX_METACARPAL_EXT];
			proximal = jointLocations[XR_HAND_JOINT_INDEX_PROXIMAL_EXT];
			tip = jointLocations[XR_HAND_JOINT_INDEX_TIP_EXT];
			break;
		case 2: // middle
			metacarpal = jointLocations[XR_HAND_JOINT_MIDDLE_METACARPAL_EXT];
			proximal = jointLocations[XR_HAND_JOINT_MIDDLE_PROXIMAL_EXT];
			tip = jointLocations[XR_HAND_JOINT_MIDDLE_TIP_EXT];
			break;
		case 3: // ring
			metacarpal = jointLocations[XR_HAND_JOINT_RING_METACARPAL_EXT];
			proximal = jointLocations[XR_HAND_JOINT_RING_PROXIMAL_EXT];
			tip = jointLocations[XR_HAND_JOINT_RING_TIP_EXT];
			break;
		case 4: // little
			metacarpal = jointLocations[XR_HAND_JOINT_LITTLE_METACARPAL_EXT];
			proximal = jointLocations[XR_HAND_JOINT_LITTLE_PROXIMAL_EXT];
			tip = jointLocations[XR_HAND_JOINT_LITTLE_TIP_EXT];
			break;
		default:
			break;
		}

		glm::vec3 metacarpalProximalDelta = X2G_v3f(metacarpal.pose.position) - X2G_v3f(proximal.pose.position);
		glm::vec3 tipProximalDelta = X2G_v3f(tip.pose.position) - X2G_v3f(proximal.pose.position);

		float dot = glm::dot(metacarpalProximalDelta, tipProximalDelta);
		float a = glm::length(metacarpalProximalDelta);
		float b = glm::length(tipProximalDelta);
		// dot = a * b * cos(ang)

		float curl;
		if (a == 0.0f || b == 0.0f) {
			// That's probably not meant to happen? But if two joints really do
			// coincide then that probably means the hand is curled up
			curl = 1.0f;
		} else {
			// Find the angle between these three joints
			float angCos = dot / (a * b);
			if (angCos < -1.0f)
				angCos = -1.0f;
			if (angCos > 1.0f)
				angCos = 1.0f;
			float ang = acosf(angCos);
			curl = 1.0f - (ang / std::numbers::pi_v<float>);
		}

		pSkeletalSummaryData->flFingerCurl[i] = curl;
	}

	for (int i = 0; i < 4; ++i) {
		OOVR_LOG_ONCE("Finger splay hardcoded at 0.2");
		pSkeletalSummaryData->flFingerSplay[i] = 0.2f; // TODO
	}

	return vr::VRInputError_None;
}

EVRInputError BaseInput::GetSkeletalSummaryData(VRActionHandle_t action, VRSkeletalSummaryData_t* pSkeletalSummaryData)
{
	return GetSkeletalSummaryData(action, VRSummaryType_FromDevice, pSkeletalSummaryData);
}
EVRInputError BaseInput::GetSkeletalBoneDataCompressed(VRActionHandle_t action, EVRSkeletalTransformSpace eTransformSpace,
    EVRSkeletalMotionRange eMotionRange, VR_OUT_BUFFER_COUNT(unCompressedSize) void* pvCompressedData, uint32_t unCompressedSize,
    uint32_t* punRequiredCompressedSize, VRInputValueHandle_t ulRestrictToDevice)
{
	// OpenComposite's own format (CompressBones): only DecompressSkeletalBoneData here reads it. No bones (a bad
	// action, no hand): no size either, as SteamVR answers
	if (punRequiredCompressedSize)
		*punRequiredCompressedSize = 0;
	BoneArray bones{};
	EVRInputError err = GetSkeletalBoneData(action, eTransformSpace, eMotionRange, bones.data(), (uint32_t)bones.size());
	if (err != VRInputError_None)
		return err;
	if (punRequiredCompressedSize)
		*punRequiredCompressedSize = kCompressedBonesSize;
	if (!pvCompressedData || unCompressedSize < kCompressedBonesSize)
		return VRInputError_BufferTooSmall;
	CompressBones(eTransformSpace, eMotionRange, bones.data(), pvCompressedData, unCompressedSize);
	return VRInputError_None;
}
EVRInputError BaseInput::GetSkeletalBoneDataCompressed(VRActionHandle_t action, EVRSkeletalMotionRange eMotionRange,
    VR_OUT_BUFFER_COUNT(unCompressedSize) void* pvCompressedData, uint32_t unCompressedSize, uint32_t* punRequiredCompressedSize)
{
	// IVRInput_003/004: bones in parent space, as SteamVR gave them then
	return GetSkeletalBoneDataCompressed(action, VRSkeletalTransformSpace_Parent, eMotionRange, pvCompressedData, unCompressedSize,
	    punRequiredCompressedSize, vr::k_ulInvalidInputValueHandle);
}
EVRInputError BaseInput::GetDigitalActionData(VRActionHandle_t action, InputDigitalActionData_t* pActionData, uint32_t unActionDataSize)
{
	return GetDigitalActionData(action, pActionData, unActionDataSize, vr::k_ulInvalidInputValueHandle);
}

EVRInputError BaseInput::GetAnalogActionData(VRActionHandle_t action, InputAnalogActionData_t* pActionData, uint32_t unActionDataSize)
{
	return GetAnalogActionData(action, pActionData, unActionDataSize, vr::k_ulInvalidInputValueHandle);
}

EVRInputError BaseInput::GetPoseActionData(VRActionHandle_t action, ETrackingUniverseOrigin eOrigin, float fPredictedSecondsFromNow,
    InputPoseActionData_t* pActionData, uint32_t unActionDataSize)
{
	return GetPoseActionData(action, eOrigin, fPredictedSecondsFromNow, pActionData, unActionDataSize, vr::k_ulInvalidInputValueHandle);
}

EVRInputError BaseInput::TriggerHapticVibrationAction(VRActionHandle_t action, float fStartSecondsFromNow, float fDurationSeconds,
    float fFrequency, float fAmplitude)
{
	return TriggerHapticVibrationAction(action, fStartSecondsFromNow, fDurationSeconds, fFrequency, fAmplitude, vr::k_ulInvalidInputValueHandle);
}

EVRInputError BaseInput::GetSkeletalActionData(VRActionHandle_t action, EVRSkeletalTransformSpace eBoneParent, float fPredictedSecondsFromNow,
    InputSkeletalActionData_t* pActionData, uint32_t unActionDataSize, VRBoneTransform_t* pTransformArray, uint32_t unTransformArrayCount)
{
	EVRInputError err = GetSkeletalActionData(action, pActionData, unActionDataSize);
	if (err != vr::VRInputError_None)
		return err;
	// A caller that only wants the action data passes no bones
	if (!pTransformArray && unTransformArrayCount == 0)
		return vr::VRInputError_None;
	return GetSkeletalBoneData(action, eBoneParent, vr::VRSkeletalMotionRange_WithController, pTransformArray, unTransformArrayCount);
}

EVRInputError BaseInput::GetSkeletalActionDataCompressed(VRActionHandle_t action, EVRSkeletalTransformSpace eBoneParent,
    float fPredictedSecondsFromNow, void* pvCompressedData, uint32_t unCompressedSize, uint32_t* punRequiredCompressedSize)
{
	return GetSkeletalBoneDataCompressed(action, vr::VRSkeletalMotionRange_WithController, pvCompressedData, unCompressedSize,
	    punRequiredCompressedSize);
}

EVRInputError BaseInput::UncompressSkeletalActionData(void* pvCompressedBuffer, uint32_t unCompressedBufferSize,
    EVRSkeletalTransformSpace* peBoneParent, VRBoneTransform_t* pTransformArray, uint32_t unTransformArrayCount)
{
	return DecompressSkeletalBoneData(pvCompressedBuffer, unCompressedBufferSize, peBoneParent, pTransformArray, unTransformArrayCount);
}

EVRInputError BaseInput::DecompressSkeletalBoneData(void* pvCompressedBuffer, uint32_t unCompressedBufferSize,
    EVRSkeletalTransformSpace* peTransformSpace, VR_ARRAY_COUNT(unTransformArrayCount) VRBoneTransform_t* pTransformArray,
    uint32_t unTransformArrayCount)
{
	// The older call: the bones in the space they were compressed in, and which that was
	if (!pTransformArray || unTransformArrayCount != eBone_Count)
		return VRInputError_InvalidParam;
	return DecompressBones(pvCompressedBuffer, unCompressedBufferSize, std::nullopt, pTransformArray, peTransformSpace);
}
EVRInputError BaseInput::DecompressSkeletalBoneData(const void* pvCompressedBuffer, uint32_t unCompressedBufferSize, EVRSkeletalTransformSpace eTransformSpace,
    VR_ARRAY_COUNT(unTransformArrayCount) VRBoneTransform_t* pTransformArray, uint32_t unTransformArrayCount)
{
	if (!pTransformArray || unTransformArrayCount != eBone_Count)
		return VRInputError_InvalidParam;
	return DecompressBones(pvCompressedBuffer, unCompressedBufferSize, eTransformSpace, pTransformArray);
}

void BaseInput::ApplyHaptic(const PendingHaptic& pulse)
{
	if (pulse.action == XR_NULL_HANDLE)
		return; // its action couldn't be made
	XrHapticActionInfo info = { XR_TYPE_HAPTIC_ACTION_INFO };
	info.action = pulse.action;
	info.subactionPath = pulse.subactionPath;
	XrHapticVibration vibration = { XR_TYPE_HAPTIC_VIBRATION };
	vibration.frequency = pulse.frequency;
	vibration.duration = pulse.duration;
	vibration.amplitude = pulse.amplitude;
	OC_XR_LOG(xrApplyHapticFeedback(xr_session.get(), &info, (XrHapticBaseHeader*)&vibration));
}

void BaseInput::FlushDueHaptics()
{
	std::vector<PendingHaptic> due;
	{
		std::lock_guard<std::mutex> queueGuard(hapticQueueLock);
		if (pendingHaptics.empty())
			return;
		const double now = EvaluatorClock();
		auto firstLater = std::stable_partition(pendingHaptics.begin(), pendingHaptics.end(), [&](const PendingHaptic& p) { return p.due <= now; });
		due.assign(pendingHaptics.begin(), firstLater);
		pendingHaptics.erase(pendingHaptics.begin(), firstLater);
	}
	for (const PendingHaptic& pulse : due)
		ApplyHaptic(pulse);
}

EVRInputError BaseInput::TriggerHapticVibrationAction(VRActionHandle_t action, float fStartSecondsFromNow, float fDurationSeconds,
    float fFrequency, float fAmplitude, VRInputValueHandle_t ulRestrictToDevice)
{
	auto sessionLock = xr_session.lock_shared(); // held while reading: a session restart waits for it
	GET_ACTION_FROM_HANDLE(act, action);

	if (act->type != ActionType::Vibration) {
		OOVR_LOGF("Cannot trigger vibration on non-vibration (type=%d) action '%s'", static_cast<int>(act->type), act->fullName.c_str());
		return VRInputError_WrongType;
	}

	// A device that isn't a hand has nothing to vibrate
	int restrictedHand = -1;
	if (ulRestrictToDevice != vr::k_ulInvalidInputValueHandle) {
		const InputValueHandle* ivh = cast_IVH(ulRestrictToDevice);
		if (!ivh)
			return VRInputError_InvalidHandle;
		restrictedHand = 2;
		if (ivh->type == InputSource::HAND_LEFT)
			restrictedHand = 0;
		else if (ivh->type == InputSource::HAND_RIGHT)
			restrictedHand = 1;
	}

	// The active sets' haptic bindings pick the hands, each scaling the amplitude. A hand ulRestrictToDevice names,
	// else every hand it's bound on; no binding: nothing vibrates, as under SteamVR
	std::vector<BindingEvaluator::HapticTarget> targets;
	{
		std::lock_guard<std::recursive_mutex> evalGuard(evaluatorLock);
		for (const BindingEvaluator* ev : evaluatedThisSync)
			for (BindingEvaluator::HapticTarget& t : ev->HapticsFor(act->manifestName))
				targets.push_back(std::move(t));
	}
	const bool delayed = std::isfinite(fStartSecondsFromNow) && fStartSecondsFromNow > 0;
	for (size_t i = 0; i < allSubactionPathNames.size() && i < allSubactionPaths.size(); i++) {
		if (restrictedHand >= 0 && (size_t)restrictedHand != i)
			continue;
		auto target = std::find_if(targets.begin(), targets.end(),
		    [&](const BindingEvaluator::HapticTarget& t) { return t.path.starts_with(allSubactionPathNames[i] + "/"); });
		if (target == targets.end())
			continue;
		PendingHaptic pulse;
		pulse.action = act->xr;
		pulse.subactionPath = allSubactionPaths[i];
		pulse.frequency = fFrequency > 0 && std::isfinite(fFrequency) ? fFrequency : XR_FREQUENCY_UNSPECIFIED;
		pulse.duration = HapticDuration(fDurationSeconds);
		pulse.amplitude = HapticAmplitude(fAmplitude, target->amplitudeScale);
		if (delayed) {
			// At most a minute ahead, and a bounded queue: a game asking every frame doesn't grow it without end
			pulse.due = EvaluatorClock() + std::min(fStartSecondsFromNow, 60.0f);
			std::lock_guard<std::mutex> queueGuard(hapticQueueLock);
			if (pendingHaptics.size() >= 256)
				pendingHaptics.erase(pendingHaptics.begin());
			pendingHaptics.push_back(pulse);
		} else {
			ApplyHaptic(pulse);
		}
	}
	return VRInputError_None;
}

EVRInputError BaseInput::GetActionOrigins(VRActionSetHandle_t actionSetHandle, VRActionHandle_t digitalActionHandle,
    VR_ARRAY_COUNT(originOutCount) VRInputValueHandle_t* originsOut, uint32_t originOutCount)
{
	auto sessionLock = xr_session.lock_shared(); // held while reading: a session restart waits for it
	GET_ACTION_FROM_HANDLE(act, digitalActionHandle);
	if (!originsOut && originOutCount)
		return VRInputError_InvalidParam;
	// No set: no origins, and no error, as SteamVR answers
	if (actionSetHandle == vr::k_ulInvalidActionSetHandle) {
		if (originsOut)
			ZeroMemory(originsOut, originOutCount * sizeof(*originsOut));
		return VRInputError_None;
	}
	GET_ACTION_SET_FROM_HANDLE(set, actionSetHandle);

	if (act->set != set) {
		// The action isn't in that set: no origins for it there
		OOVR_LOGF("GetActionOrigins: set mismatch %s vs %s", set->name.c_str(), act->fullName.c_str());
		return VRInputError_InvalidHandle;
	}

	if (act->type == ActionType::EyeTracking)
		return VRInputError_InvalidHandle;

	if (originsOut)
		ZeroMemory(originsOut, originOutCount * sizeof(*originsOut));

	std::set<std::string> sources;
	XrBoundSourcesForActionEnumerateInfo info = { XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO };
	info.action = act->xr;

	// Each hand's binding file (or the hand itself, with no file) gives the inputs, or for a pose and a skeleton the
	// hands, that write the action
	if (act->type == ActionType::Skeleton || act->type == ActionType::Pose) {
		std::lock_guard<std::recursive_mutex> evalGuard(evaluatorLock);
		bool anyFile = false;
		for (size_t h = 0; h < allSubactionPathNames.size() && h < handInteractionProfiles.size(); h++) {
			const InteractionProfile* profile = handInteractionProfiles[h];
			if (!profile) {
				auto device = BackendManager::Instance().GetBackendInstance()->GetDeviceByHand(static_cast<ITrackedDevice::TrackedDeviceType>(h));
				profile = device ? device->GetInteractionProfile() : nullptr;
			}
			auto file = bindingFiles.find(profile);
			if (file == bindingFiles.end())
				continue;
			anyFile = true;
			for (const std::string& device : PoseDevicesForAction(file->second, act->manifestName))
				sources.insert(device);
		}
		if (!anyFile) {
			if (act->type == ActionType::Skeleton)
				sources.insert(act->skeletalHand == ITrackedDevice::HAND_LEFT ? "/user/hand/left" : "/user/hand/right");
			else
				for (const std::string& hand : allSubactionPathNames)
					sources.insert(hand);
		}
	}

	// The inputs the binding files bind it on; OpenXR's bound sources for a vibration, or if no controller in use has a file
	std::optional<std::vector<ActionBinding>> fromFiles = BindingsFromFiles(*act);
	if (fromFiles)
		for (const ActionBinding& b : *fromFiles)
			sources.insert(b.device + b.input);

	// The runtime's bound sources: their count first, then that many
	std::vector<XrPath> tmp;
	uint32_t count = 0;
	const bool isPose = act->type == ActionType::Skeleton || act->type == ActionType::Pose;
	if (!fromFiles && !isPose && info.action != XR_NULL_HANDLE) {
		XrResult enumResult = xrEnumerateBoundSourcesForAction(xr_session.get(), &info, 0, &count, nullptr);
		if (XR_SUCCEEDED(enumResult) && count > 0) {
			tmp.resize(count);
			enumResult = xrEnumerateBoundSourcesForAction(xr_session.get(), &info, count, &count, tmp.data());
		}
		if (XR_FAILED(enumResult)) {
			OOVR_LOG_ONCE("GetActionOrigins: xrEnumerateBoundSourcesForAction failed");
			count = 0;
		}
		count = std::min<uint32_t>(count, (uint32_t)tmp.size());
	}

	// Now for each source its input (a vibration's is its hand)
	char buff[XR_MAX_PATH_LENGTH + 1] = {};
	for (uint32_t i = 0; i < count; i++) {
		uint32_t len = 0;
		if (XR_FAILED(xrPathToString(xr_instance, tmp[i], XR_MAX_PATH_LENGTH, &len, buff)) || len == 0)
			continue;

		// len counts the terminator
		std::string path(buff, strnlen(buff, std::min<size_t>(len, XR_MAX_PATH_LENGTH)));
		if (path.find("/input/") != std::string::npos) {
			sources.insert(OriginFromInputPath(path));
			continue;
		}
		size_t endOfHandPos = path.find('/', strlen("/user/hand/") + 1);
		if (endOfHandPos != std::string::npos)
			path.erase(endOfHandPos);
		sources.insert(std::move(path));
	}

	// Copy out the sources
	uint32_t i = 0;
	for (const std::string& path : sources) {
		if (i >= originOutCount)
			return vr::VRInputError_MaxCapacityReached; // TODO check this is correct
		if (GetInputSourceHandle(path.c_str(), &originsOut[i]) != vr::VRInputError_None)
			continue;
		i++;
	}

	return VRInputError_None;
}

EVRInputError BaseInput::GetOriginLocalizedName(VRInputValueHandle_t origin, VR_OUT_STRING() char* pchNameArray, uint32_t unNameArraySize)
{
	return GetOriginLocalizedName(origin, pchNameArray, unNameArraySize, VRInputString_All);
}

EVRInputError BaseInput::GetOriginLocalizedName(VRInputValueHandle_t origin, VR_OUT_STRING() char* pchNameArray, uint32_t unNameArraySize,
    uint64_t unStringSectionsToInclude)
{
	auto sessionLock = xr_session.lock_shared(); // held while reading: a session restart waits for it
	if (origin == vr::k_ulInvalidInputValueHandle)
		return vr::VRInputError_InvalidHandle;

	std::shared_ptr<ITrackedDevice> dev = ivhToDev(origin);

	if (!dev)
		return VRInputError_InvalidHandle;

	if (!pchNameArray || unNameArraySize == 0)
		return VRInputError_MaxCapacityReached;

	const InputValueHandle* ivh = cast_IVH(origin);
	const InteractionProfile* profile = dev->GetInteractionProfile();
	std::optional<const char*> controllerType = profile ? profile->GetOpenVRName() : std::nullopt;
	int hand = dev->GetHand() == ITrackedDevice::HAND_LEFT ? 0 : dev->GetHand() == ITrackedDevice::HAND_RIGHT ? 1 : -1;
	std::string name = OriginLocalizedName(hand, controllerType ? *controllerType : "", ivh->path,
	    (unStringSectionsToInclude & VRInputString_Hand) != 0, (unStringSectionsToInclude & VRInputString_ControllerType) != 0,
	    (unStringSectionsToInclude & VRInputString_InputSource) != 0);

	const char* str = name.c_str();

	size_t i;
	for (i = 0; str[i] && i < unNameArraySize - 1; ++i) {
		pchNameArray[i] = str[i];
	}
	pchNameArray[i] = 0;

	return str[i] ? VRInputError_MaxCapacityReached : VRInputError_None;
}

EVRInputError BaseInput::GetOriginTrackedDeviceInfo(VRInputValueHandle_t origin, InputOriginInfo_t* info, uint32_t unOriginInfoSize)
{
	// A null or wrong-sized struct is an argument error, as SteamVR answers (VRInputError_InvalidParam)
	if (!info || unOriginInfoSize != sizeof(InputOriginInfo_t))
		return VRInputError_InvalidParam;
	memset(info, 0, unOriginInfoSize);

	if (origin == vr::k_ulInvalidInputValueHandle)
		return vr::VRInputError_InvalidHandle;

	std::shared_ptr<ITrackedDevice> dev = ivhToDev(origin);

	if (!dev)
		return vr::VRInputError_InvalidHandle;

	info->trackedDeviceIndex = dev->DeviceIndex();
	info->devicePath = origin;
	// The component of the render model the origin's input moves; none for a hand
	strcpy_arr(info->rchRenderModelComponentName, OriginComponentName(cast_IVH(origin)->path).c_str());

	return VRInputError_None;
}

/** Retrieves useful information about the bindings for an action */
EVRInputError BaseInput::GetActionBindingInfo(VRActionHandle_t actionHandle, OOVR_InputBindingInfo_t* bindingInfo,
    uint32_t unBindingInfoSize, uint32_t unBindingInfoCount, uint32_t* punReturnedBindingInfoCount)
{
	auto sessionLock = xr_session.lock_shared(); // held while reading: a session restart waits for it
	if (punReturnedBindingInfoCount)
		*punReturnedBindingInfoCount = 0;
	// A null or wrong-sized struct is an argument error, as SteamVR answers (VRInputError_InvalidParam)
	if (unBindingInfoSize != sizeof(OOVR_InputBindingInfo_t) || (unBindingInfoCount && !bindingInfo))
		return VRInputError_InvalidParam;
	if (bindingInfo)
		memset(bindingInfo, 0, (size_t)unBindingInfoSize * unBindingInfoCount);

	// FIXME support any number of sources
	// TODO does this support passing in unBindingInfoSize=0 and reading the required size? Check with SteamVR.
	GET_ACTION_FROM_HANDLE(action, actionHandle);
	if (action->type == ActionType::EyeTracking)
		return VRInputError_None;

	// From the controllers' binding files, as SteamVR answers: OpenXR's bound sources have no mode or slot, and
	// nothing for a d-pad. A pose and a skeleton are listed too, and a vibration with no device or input path.
	if (std::optional<std::vector<ActionBinding>> fromFiles = BindingsFromFiles(*action, true)) {
		uint32_t written = 0;
		for (const ActionBinding& b : *fromFiles) {
			if (written >= unBindingInfoCount)
				break;
			OOVR_InputBindingInfo_t& info = bindingInfo[written++];
			strcpy_arr(info.rchDevicePathName, b.device.c_str());
			strcpy_arr(info.rchInputPathName, b.input.c_str());
			strcpy_arr(info.rchModeName, b.mode.c_str());
			strcpy_arr(info.rchSlotName, b.slot.c_str());
			strcpy_arr(info.rchInputSourceType, b.sourceType.c_str());
		}
		if (punReturnedBindingInfoCount)
			*punReturnedBindingInfoCount = written;
		return VRInputError_None;
	}

	// Only a vibration has an OpenXR action of its own
	if (action->xr == XR_NULL_HANDLE)
		return VRInputError_None;

	XrBoundSourcesForActionEnumerateInfo enumInfo = { XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO };
	enumInfo.action = action->xr;
	uint32_t sourcesCount = 0;
	OC_XR_LOG(xrEnumerateBoundSourcesForAction(xr_session.get(), &enumInfo, 0, &sourcesCount, nullptr));
	std::vector<XrPath> boundActionPaths(sourcesCount);
	OC_XR_LOG(xrEnumerateBoundSourcesForAction(xr_session.get(), &enumInfo, boundActionPaths.size(), &sourcesCount, boundActionPaths.data()));

	// TODO should we return an error if there are no sources bound?

	// A vibration's binding is told with every field empty, as SteamVR answers: one per bound output (a source that
	// isn't one is left out)
	uint32_t written = 0;
	for (size_t i = 0; i < boundActionPaths.size() && written < unBindingInfoCount; i++) {
		char chars[XR_MAX_PATH_LENGTH] = {};
		uint32_t len = 0;
		if (XR_FAILED(xrPathToString(xr_instance, boundActionPaths[i], sizeof(chars), &len, chars)))
			continue;
		const std::string path(chars, strnlen(chars, sizeof(chars)));
		if (path.starts_with("/user/") && path.find("/output/") != std::string::npos)
			written++;
	}
	if (punReturnedBindingInfoCount)
		*punReturnedBindingInfoCount = written;

	return VRInputError_None;
}

EVRInputError BaseInput::ShowActionOrigins(VRActionSetHandle_t actionSetHandle, VRActionHandle_t ulActionHandle)
{
	// Nothing is shown, and the game goes on as if it were
	OC_UNSUPPORTED(VRInputError_None, "ShowActionOrigins: nothing is shown");
}
EVRInputError BaseInput::ShowBindingsForActionSet(VR_ARRAY_COUNT(unSetCount) VRActiveActionSet_t* pSets, uint32_t unSizeOfVRSelectedActionSet_t,
    uint32_t unSetCount, VRInputValueHandle_t originToHighlight)
{
	// Nothing is shown, and the game goes on as if it were
	OC_UNSUPPORTED(VRInputError_None, "ShowBindingsForActionSet: nothing is shown");
}

EVRInputError BaseInput::GetComponentStateForBinding(const char* pchRenderModelName, const char* pchComponentName,
    const OOVR_InputBindingInfo_t* pOriginInfo, uint32_t unBindingInfoSize, uint32_t unBindingInfoCount,
    vr::RenderModel_ComponentState_t* pComponentState)
{
	// No render model component follows a binding yet
	if (pComponentState)
		*pComponentState = {};
	return VRInputError_NoData;
}

bool BaseInput::IsUsingLegacyInput()
{
	return usingLegacyInput;
}

// Interestingly enough this was added to IVRInput_007 without bumping the version number - that's fine since it's
// at the end of the vtable, but it's interesting that the version has always been bumped for this in the past.
EVRInputError BaseInput::OpenBindingUI(const char* pchAppKey, VRActionSetHandle_t ulActionSetHandle,
    VRInputValueHandle_t ulDeviceHandle, bool bShowOnDesktop)
{
	// The binding editor set in openlamina.ini, if any; SteamVR answers NoSteam without Steam
	std::string editor = oovr_global_configuration.BindingEditor();
	if (editor.empty())
		OC_UNSUPPORTED(VRInputError_NoSteam, "OpenBindingUI: no bindingEditor in openlamina.ini");

	std::string key = pchAppKey && pchAppKey[0] ? std::string(pchAppKey) : CurrentProcessAppKey();
	ActionSet* set = ulActionSetHandle != vr::k_ulInvalidActionSetHandle ? cast_ASH(ulActionSetHandle) : nullptr;
	InputValueHandle* device = cast_IVH(ulDeviceHandle);
	// Each value one argument of its own, whatever it holds
	std::string args = "--app-key " + QuoteArgument(key) + " --action-set " + QuoteArgument(set ? set->fullName : "")
	    + " --device " + QuoteArgument(device ? device->path : "");
	std::filesystem::path binary = PathFromUtf8(editor);
	if (!LaunchProcess(binary, args, binary.parent_path()))
		OC_UNSUPPORTED(VRInputError_NoSteam, "OpenBindingUI: the binding editor %s didn't start", editor.c_str());
	return VRInputError_None;
}

EVRInputError BaseInput::GetBindingVariant(vr::VRInputValueHandle_t ulDevicePath, char* pchVariantArray, uint32_t unVariantArraySize)
{
	if (!pchVariantArray || unVariantArraySize == 0) {
		return VRInputError_MaxCapacityReached;
	}

	// No binding file carries a variant: the empty string SteamVR answers for bindings without one
	*pchVariantArray = 0;
	return VRInputError_None;
}

BaseInput::Action* BaseInput::cast_AH(VRActionHandle_t handle)
{
	return actions.LookupItem((RegHandle)handle);
}

BaseInput::ActionSet* BaseInput::cast_ASH(VRActionSetHandle_t handle)
{
	return actionSets.LookupItem((RegHandle)handle);
}

BaseInput::InputValueHandle* BaseInput::cast_IVH(VRInputValueHandle_t handle)
{
	// nullptr for k_ulInvalidInputValueHandle and for anything GetInputSourceHandle didn't hand out
	if (handle == vr::k_ulInvalidInputValueHandle)
		return nullptr;
	std::lock_guard<std::mutex> guard(LiveHandlesLock());
	if (!LiveHandles().count((const void*)handle))
		return nullptr;
	return (InputValueHandle*)handle;
}

std::shared_ptr<ITrackedDevice> BaseInput::ivhToDev(VRInputValueHandle_t handle)
{
	const InputValueHandle* ivh = cast_IVH(handle);
	if (!ivh)
		return nullptr;

	ITrackedDevice::TrackedDeviceType hand = ITrackedDevice::HAND_NONE;
	switch (ivh->type) {
	case InputSource::HAND_LEFT:
		hand = ITrackedDevice::HAND_LEFT;
		break;
	case InputSource::HAND_RIGHT:
		hand = ITrackedDevice::HAND_RIGHT;
		break;
	default:
		// A tracker by the path SteamVR binds its pose on
		if (std::optional<ViveTrackerRole> role = ViveTrackerRoleForSteamVRPath(ivh->devicePathString))
			return TrackerForRole(*role);
		OOVR_SOFT_ABORTF("Cannot get invalid device from handle '%s'", ivh->path.c_str());
		return nullptr;
	}

	return BackendManager::Instance().GetDeviceByHand(hand);
}

bool BaseInput::checkRestrictToDevice(vr::VRInputValueHandle_t restrictToDevice, XrPath subactionPath)
{
	if (restrictToDevice == vr::k_ulInvalidInputValueHandle)
		return true;

	const InputValueHandle* ivh = cast_IVH(restrictToDevice);
	return ivh && subactionPath == ivh->devicePath;
}

bool BaseInput::GetLegacyControllerState(vr::TrackedDeviceIndex_t controllerDeviceIndex, vr::VRControllerState_t* state)
{
	if (!state)
		return false;
	*state = {};

	int hand = DeviceIndexToHandId(controllerDeviceIndex);
	if (hand == -1)
		return false;

	bool axis3Free = true, axis4Free = true;
	{
		auto sessionLock = xr_session.lock_shared(); // held while reading: a session restart waits for it

		// SteamVR's legacy binding file for the controller
		if (rawInputsSet != XR_NULL_HANDLE) {
			std::shared_ptr<ITrackedDevice> dev = BackendManager::Instance().GetDevice(controllerDeviceIndex);
			std::lock_guard<std::recursive_mutex> evalGuard(evaluatorLock);
			const InteractionProfile* profile = dev ? dev->GetInteractionProfile() : nullptr;
			if (const BindingEvaluator* legacy = LegacyEvaluatorFor(profile)) {
				const std::string handName = hand == 0 ? "left" : "right";
				ApplyLegacyOutputs(legacy->Outputs(), handName, *state);
				if (const BindingFile* file = LoadLegacyBindings(profile->GetOpenVRName().value_or("generic"))) {
					axis3Free = LegacyAxisType(*file, handName, 3) == vr::k_eControllerAxis_None;
					axis4Free = LegacyAxisType(*file, handName, 4) == vr::k_eControllerAxis_None;
				}
			}
		}
	}

	// SteamVR writes the finger curls to the axes after the grip's
	if (xr_gbl && xr_gbl->handTrackingProperties.supportsHandTracking && (axis3Free || axis4Free)) {
		VRSkeletalSummaryData_t skeletonData{};
		if (getRealSkeletalSummary((ITrackedDevice::TrackedDeviceType)hand, &skeletonData) == VRInputError_None)
			ApplyLegacyFingerCurls(skeletonData.flFingerCurl, axis3Free, axis4Free, *state);
	}

	// The packet number moves on when the state does
	std::lock_guard<std::mutex> packetGuard(legacyPacketLock);
	LegacyPacket& last = legacyPackets[hand];
	if (!last.valid || LegacyStateChanged(last.last, *state)) {
		last.packet++;
		last.last = *state;
		last.valid = true;
	}
	state->unPacketNum = last.packet;
	return true;
}

void BaseInput::TriggerLegacyHapticPulse(vr::TrackedDeviceIndex_t controllerDeviceIndex, uint64_t durationNanos)
{
	auto sessionLock = xr_session.lock_shared(); // held while reading: a session restart waits for it
	int hand = DeviceIndexToHandId(controllerDeviceIndex);
	if (hand == -1)
		return;
	LegacyControllerActions& ctrl = legacyControllers[hand];

	if (!ctrl.haptic) {
		OOVR_LOG_ONCE("Cannot trigger haptic pulse, no haptic action present");
		return;
	}

	XrHapticActionInfo info = { XR_TYPE_HAPTIC_ACTION_INFO };
	info.action = ctrl.haptic;

	XrHapticVibration vibration = { XR_TYPE_HAPTIC_VIBRATION };
	vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
	vibration.duration = durationNanos;
	vibration.amplitude = 1;

	// The legacy file's haptic for this hand, and its amplitude (none: nothing vibrates)
	std::shared_ptr<ITrackedDevice> dev = BackendManager::Instance().GetDevice(controllerDeviceIndex);
	std::vector<BindingEvaluator::HapticTarget> targets;
	{
		std::lock_guard<std::recursive_mutex> evalGuard(evaluatorLock);
		if (const BindingEvaluator* legacy = LegacyEvaluatorFor(dev ? dev->GetInteractionProfile() : nullptr))
			targets = legacy->HapticsFor(std::string("/actions/legacy/out/") + (hand == 0 ? "left" : "right") + "_haptic");
	}
	if (targets.empty())
		return;
	vibration.amplitude = std::clamp(targets.front().amplitudeScale * LegacyHapticStrength(durationNanos / 1000), 0.0f, 1.0f);

	OC_XR_LOG(xrApplyHapticFeedback(xr_session.get(), &info, (XrHapticBaseHeader*)&vibration));
}

int BaseInput::DeviceIndexToHandId(vr::TrackedDeviceIndex_t idx)
{
	std::shared_ptr<ITrackedDevice> dev = BackendManager::Instance().GetDevice(idx);
	if (!dev)
		return -1;

	ITrackedDevice::TrackedDeviceType hand = dev->GetHand();

	switch (hand) {
	case ITrackedDevice::HAND_LEFT:
		return 0;
	case ITrackedDevice::HAND_RIGHT:
		return 1;
	default:
		return -1;
	}
}

void BaseInput::GetHandSpace(vr::TrackedDeviceIndex_t index, XrSpace& space, bool aimPose)
{
	space = XR_NULL_HANDLE;

	// If the manifest isn't loaded yet (still on the first frame) return null
	if (!hasLoadedActions)
		return;

	std::shared_ptr<ITrackedDevice> dev = BackendManager::Instance().GetDevice(index);
	if (!dev)
		return;

	ITrackedDevice::TrackedDeviceType hand = dev->GetHand();
	GetHandSpace(hand, space, aimPose);
}

void BaseInput::GetHandSpace(ITrackedDevice::TrackedDeviceType hand, XrSpace& space, bool aimPose)
{
	LegacyControllerActions& ctrl = legacyControllers[hand];

	space = aimPose ? ctrl.aimPoseSpace : ctrl.gripPoseSpace;
}

bool BaseInput::AreActionsLoaded()
{
	return hasLoadedActions;
}

bool BaseInput::GetHandPointer(int hand, XrSpace base, XrTime time, HandPointer& out)
{
	out = {};
	if (hand < 0 || hand > 1)
		return false;
	auto sessionLock = xr_session.lock_shared(); // held while reading: a session restart waits for it
	// A manifest being loaded replaces the actions and spaces under this lock
	std::lock_guard<std::recursive_mutex> evalGuard(evaluatorLock);
	if (!hasLoadedActions || legacyInputsSet == XR_NULL_HANDLE || base == XR_NULL_HANDLE || !xr_session.get())
		return false;
	const LegacyControllerActions& ctrl = legacyControllers[hand];
	if (ctrl.aimPoseSpace == XR_NULL_HANDLE)
		return false;
	XrSpaceLocation location{ XR_TYPE_SPACE_LOCATION };
	if (XR_FAILED(xrLocateSpace(ctrl.aimPoseSpace, base, time, &location)))
		return false;
	const XrSpaceLocationFlags valid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	if ((location.locationFlags & valid) != valid)
		return false;
	out.aim = location.pose;

	// A float action's state (a boolean input reads as 0 or 1 through one); nothing if inactive
	auto read = [&](XrAction action, XrPath subaction) -> std::optional<float> {
		if (action == XR_NULL_HANDLE)
			return {};
		XrActionStateGetInfo getInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
		getInfo.action = action;
		getInfo.subactionPath = subaction;
		XrActionStateFloat state = { XR_TYPE_ACTION_STATE_FLOAT };
		if (XR_FAILED(xrGetActionStateFloat(xr_session.get(), &getInfo, &state)) || !state.isActive)
			return {};
		return state.currentState;
	};
	// The raw inputs: the first of each a controller has
	const XrPath subaction = (size_t)hand < allSubactionPaths.size() ? allSubactionPaths[hand] : XR_NULL_PATH;
	auto first = [&](std::initializer_list<const char*> components) -> float {
		for (const char* component : components) {
			auto action = rawActions.find(component);
			if (action == rawActions.end())
				continue;
			if (std::optional<float> v = read(action->second, subaction))
				return *v;
		}
		return 0.0f;
	};
	out.trigger = first({ "/input/trigger/value", "/input/trigger/click", "/input/select/click" });
	out.stickX = first({ "/input/thumbstick/x", "/input/trackpad/x" });
	out.stickY = first({ "/input/thumbstick/y", "/input/trackpad/y" });
	return true;
}

static std::atomic<bool> heldByDashboard{ false };

void BaseInput::SetHeldByDashboard(bool held)
{
	heldByDashboard = held;
}

bool BaseInput::HeldByDashboard()
{
	return heldByDashboard;
}

bool BaseInput::InputHeld()
{
	// The grid fully in and not asked to fade out: once the game asks for that, its input is back as the fade starts
	const CompositorFades& fades = CompositorFades::Get();
	return HeldByDashboard() || (fades.GridWanted() && fades.GridAlphaAt(CompositorClockSeconds()) >= 1.0f);
}

bool BaseInput::GetDashboardButton(bool system, int hand)
{
	if (hand < 0 || hand > 1 || (!system && hand != 0))
		return false;
	auto sessionLock = xr_session.lock_shared(); // held while reading: a session restart waits for it
	std::lock_guard<std::recursive_mutex> evalGuard(evaluatorLock);
	if (!hasLoadedActions || legacyInputsSet == XR_NULL_HANDLE || !xr_session.get())
		return false;
	auto held = [&](XrAction action, XrPath subaction) {
		if (action == XR_NULL_HANDLE)
			return false;
		XrActionStateGetInfo getInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
		getInfo.action = action;
		getInfo.subactionPath = subaction;
		XrActionStateBoolean state = { XR_TYPE_ACTION_STATE_BOOLEAN };
		return XR_SUCCEEDED(xrGetActionStateBoolean(xr_session.get(), &getInfo, &state)) && state.isActive && state.currentState;
	};
	// The controller's system button, as SteamVR's dashboard opens
	if (system)
		return held(legacyControllers[hand].dashboard, XR_NULL_PATH);
	// The left controller's menu button, from the raw inputs
	const XrPath subaction = (size_t)hand < allSubactionPaths.size() ? allSubactionPaths[hand] : XR_NULL_PATH;
	for (const char* component : { "/input/system/click", "/input/menu/click", "/input/b/click", "/input/y/click" }) {
		auto action = rawActions.find(component);
		if (action == rawActions.end())
			continue;
		// The raw inputs are float actions
		XrActionStateGetInfo getInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
		getInfo.action = action->second;
		getInfo.subactionPath = subaction;
		XrActionStateFloat state = { XR_TYPE_ACTION_STATE_FLOAT };
		// Any of them held: a controller's system button is active but never pressed on runtimes that keep it
		if (XR_SUCCEEDED(xrGetActionStateFloat(xr_session.get(), &getInfo, &state)) && state.isActive && state.currentState >= 0.5f)
			return true;
	}
	return false;
}

void BaseInput::SyncPointerActions()
{
	auto sessionLock = xr_session.lock_shared(); // held while reading: a session restart waits for it
	std::lock_guard<std::recursive_mutex> evalGuard(evaluatorLock);
	if (!hasLoadedActions || legacyInputsSet == XR_NULL_HANDLE || !xr_session.get())
		return;
	XrActiveActionSet aas[2] = { { legacyInputsSet }, { rawInputsSet } };
	XrActionsSyncInfo syncInfo = { XR_TYPE_ACTIONS_SYNC_INFO };
	syncInfo.activeActionSets = aas;
	syncInfo.countActiveActionSets = rawInputsSet != XR_NULL_HANDLE ? 2 : 1;
	OC_XR_LOG(xrSyncActions(xr_session.get(), &syncInfo));
	syncSerial++;
}

void BaseInput::TriggerHandHaptic(int hand, float seconds, float frequency, float amplitude)
{
	if (hand < 0 || hand > 1 || !oovr_global_configuration.Haptics())
		return;
	auto sessionLock = xr_session.lock_shared(); // held while reading: a session restart waits for it
	std::lock_guard<std::recursive_mutex> evalGuard(evaluatorLock);
	const LegacyControllerActions& ctrl = legacyControllers[hand];
	if (!hasLoadedActions || ctrl.haptic == XR_NULL_HANDLE || !xr_session.get())
		return;
	XrHapticActionInfo info = { XR_TYPE_HAPTIC_ACTION_INFO };
	info.action = ctrl.haptic;
	XrHapticVibration vibration = { XR_TYPE_HAPTIC_VIBRATION };
	vibration.duration = seconds > 0 && std::isfinite(seconds) ? (XrDuration)(std::min(seconds, 10.0f) * 1e9) : XR_MIN_HAPTIC_DURATION;
	vibration.frequency = frequency > 0 && std::isfinite(frequency) ? frequency : XR_FREQUENCY_UNSPECIFIED;
	vibration.amplitude = std::isfinite(amplitude) ? std::clamp(amplitude, 0.0f, 1.0f) : 1.0f;
	OC_XR_LOG(xrApplyHapticFeedback(xr_session.get(), &info, (XrHapticBaseHeader*)&vibration));
}

bool BaseInput::HasEyeGaze()
{
	std::lock_guard<std::recursive_mutex> evalGuard(evaluatorLock);
	return hasLoadedActions && eyeGazeSpace != XR_NULL_HANDLE;
}

bool BaseInput::GetEyeGaze(XrSpace base, XrTime time, XrPosef* gaze, XrSpaceLocationFlags* flags)
{
	// A manifest being loaded replaces the space under this lock
	std::lock_guard<std::recursive_mutex> evalGuard(evaluatorLock);
	if (!hasLoadedActions || eyeGazeSpace == XR_NULL_HANDLE || base == XR_NULL_HANDLE)
		return false;

	XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };
	if (XR_FAILED(xrLocateSpace(eyeGazeSpace, base, time, &location)))
		return false;
	// Not requiring tracked: a runtime holding the last gaze through a blink says valid only, and that beats a jump
	if (!(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
		return false;
	*gaze = location.pose;
	if (flags)
		*flags = location.locationFlags;
	return true;
}

ITrackedDevice::TrackedDeviceType BaseInput::ParseAndRemoveHandPrefix(std::string& toModify)
{
	static std::string leftPrefix = "/user/hand/left/";
	static std::string rightPrefix = "/user/hand/right/";

	if (toModify.starts_with(leftPrefix)) {
		toModify.erase(toModify.begin(), toModify.begin() + (int)leftPrefix.size());
		return ITrackedDevice::HAND_LEFT;
	}

	if (toModify.starts_with(rightPrefix)) {
		toModify.erase(toModify.begin(), toModify.begin() + (int)rightPrefix.size());
		return ITrackedDevice::HAND_RIGHT;
	}

	return ITrackedDevice::HAND_NONE;
}

EVRInputError BaseInput::GetEyeTrackingDataRelativeToNow(VRActionHandle_t action, ETrackingUniverseOrigin eOrigin,
    float fPredictedSecondsFromNow, VREyeTrackingData_t* pEyeTrackingData, uint32_t ulEyeTrackingDataSize)
{
	// So many seconds from now, where the runtime can say what time it is; otherwise at the frame being predicted
	XrTime time = 0;
	if (std::isfinite(fPredictedSecondsFromNow)) {
		if (XrTime now = xr_time_now(); now > 0)
			time = now + (XrTime)(std::clamp((double)fPredictedSecondsFromNow, -1.0, 1.0) * 1e9);
	}
	return GetEyeTrackingDataAt(action, eOrigin, time, pEyeTrackingData, ulEyeTrackingDataSize);
}

EVRInputError BaseInput::GetEyeTrackingDataAt(VRActionHandle_t action, ETrackingUniverseOrigin eOrigin,
    XrTime time, VREyeTrackingData_t* pEyeTrackingData, uint32_t ulEyeTrackingDataSize)
{
	GET_ACTION_FROM_HANDLE(act, action);
	if (ulEyeTrackingDataSize != sizeof(*pEyeTrackingData))
		return VRInputError_InvalidParam;
	if (!pEyeTrackingData)
		return VRInputError_None;

	// As SteamVR answers: in the space asked for; with no eye tracker behind the action (or for an action of
	// another type) inactive, at the head; with one, active and as valid as the tracker's gaze.
	*pEyeTrackingData = {};
	auto lock = xr_session.lock_shared();
	if (!xr_gbl)
		return VRInputError_None;
	XrSpace base = xr_space_from_tracking_origin(eOrigin);
	if (time <= 1)
		time = xr_gbl->GetBestTime();
	XrSpaceLocation head = { XR_TYPE_SPACE_LOCATION };
	bool headValid = XR_SUCCEEDED(xrLocateSpace(xr_gbl->viewSpace, base, time, &head)) && (head.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
	if (headValid)
		pEyeTrackingData->vGazeOrigin = pEyeTrackingData->vGazeTarget = G2S_v3f(X2G_v3f(head.pose.position));
	if (act->type != ActionType::EyeTracking || !HasEyeGaze())
		return VRInputError_None;
	pEyeTrackingData->bActive = true;

	XrPosef gaze;
	XrSpaceLocationFlags flags = 0;
	if (!GetEyeGaze(base, time, &gaze, &flags))
		return VRInputError_None;
	// A runtime may give the direction without the eye's position: the head's is the nearest
	if (!(flags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
		if (!headValid)
			return VRInputError_None;
		gaze.position = head.pose.position;
	}

	// OpenXR's gaze has no depth, so the fixation point is far along it: its parallax from the eyes' offset is
	// negligible, and a game sees nothing near. Not infinite, which would turn a game's vector maths into NaNs.
	constexpr float kFixationDistance = 100.0f;
	glm::vec3 origin = X2G_v3f(gaze.position);
	glm::vec3 target = origin + X2G_quat(gaze.orientation) * glm::vec3(0, 0, -kFixationDistance);
	pEyeTrackingData->bValid = true;
	pEyeTrackingData->bTracked = (flags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) != 0;
	pEyeTrackingData->vGazeOrigin = G2S_v3f(origin);
	pEyeTrackingData->vGazeTarget = G2S_v3f(target);
	return VRInputError_None;
}

EVRInputError BaseInput::GetEyeTrackingDataForNextFrame(VRActionHandle_t action, ETrackingUniverseOrigin eOrigin,
    VREyeTrackingData_t* pEyeTrackingData, uint32_t ulEyeTrackingDataSize)
{
	// The frame the last WaitGetPoses was for
	return GetEyeTrackingDataAt(action, eOrigin, 0, pEyeTrackingData, ulEyeTrackingDataSize);
}
