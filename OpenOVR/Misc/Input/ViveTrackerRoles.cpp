#include "stdafx.h"

#include "ViveTrackerRoles.h"

const std::vector<ViveTrackerRole>& ViveTrackerRoles()
{
	// The roles of /interaction_profiles/htc/vive_tracker_htcx
	static const std::vector<ViveTrackerRole> roles = {
		{ "handheld_object", "" },
		{ "left_foot", "/user/foot/left" },
		{ "right_foot", "/user/foot/right" },
		{ "left_shoulder", "/user/shoulder/left" },
		{ "right_shoulder", "/user/shoulder/right" },
		{ "left_elbow", "/user/elbow/left" },
		{ "right_elbow", "/user/elbow/right" },
		{ "left_knee", "/user/knee/left" },
		{ "right_knee", "/user/knee/right" },
		{ "left_wrist", "/user/wrist/left" },
		{ "right_wrist", "/user/wrist/right" },
		{ "left_ankle", "/user/ankle/left" },
		{ "right_ankle", "/user/ankle/right" },
		{ "waist", "/user/waist" },
		{ "chest", "/user/chest" },
		{ "camera", "/user/camera" },
		{ "keyboard", "/user/keyboard" },
	};
	return roles;
}

std::string ViveTrackerRolePath(const ViveTrackerRole& role)
{
	return std::string("/user/vive_tracker_htcx/role/") + role.name;
}

std::string ViveTrackerControllerType(const ViveTrackerRole& role)
{
	return std::string("vive_tracker_") + role.name;
}

std::optional<ViveTrackerRole> ViveTrackerRoleForXrPath(const std::string& rolePath)
{
	for (const ViveTrackerRole& role : ViveTrackerRoles())
		if (ViveTrackerRolePath(role) == rolePath)
			return role;
	return std::nullopt;
}

std::optional<ViveTrackerRole> ViveTrackerRoleForSteamVRPath(const std::string& path)
{
	for (const ViveTrackerRole& role : ViveTrackerRoles()) {
		const std::string base = role.steamvrPath;
		if (!base.empty() && path.starts_with(base) && (path.size() == base.size() || path[base.size()] == '/'))
			return role;
	}
	return std::nullopt;
}
