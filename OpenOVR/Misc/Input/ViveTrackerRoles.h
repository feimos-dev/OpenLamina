#pragma once

// The roles a Vive tracker can have under XR_HTCX_vive_tracker_interaction, with what SteamVR calls them: the path
// a game's binding file binds a tracker's pose on (/user/waist) and the tracker's controller type.

#include <optional>
#include <string>
#include <vector>

struct ViveTrackerRole {
	const char* name; // the role's part of the OpenXR path: waist, left_foot
	const char* steamvrPath; // /user/waist, /user/foot/left; empty for one SteamVR binds nothing on
};

/** Every role, in the extension's order */
const std::vector<ViveTrackerRole>& ViveTrackerRoles();

/** The role's OpenXR user path: /user/vive_tracker_htcx/role/waist */
std::string ViveTrackerRolePath(const ViveTrackerRole& role);

/** The role's controller type, as SteamVR names a tracker with that role: vive_tracker_waist */
std::string ViveTrackerControllerType(const ViveTrackerRole& role);

/** The role of an OpenXR role path, or of the path a game binds a tracker's pose on (and anything below it) */
std::optional<ViveTrackerRole> ViveTrackerRoleForXrPath(const std::string& rolePath);
std::optional<ViveTrackerRole> ViveTrackerRoleForSteamVRPath(const std::string& path);
