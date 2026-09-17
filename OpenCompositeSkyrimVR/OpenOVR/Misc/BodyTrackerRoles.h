#pragma once

#include <cstdint>
#include <string_view>

// The full set of body tracker roles VDXR / SteamVR expose through
// XR_HTCX_vive_tracker_interaction. Shared by BaseInput (actions/bindings)
// and XrGenericTracker/XrBackend (device exposure).
//
// Which roles are actually enabled comes from the ini:
//   [input] bodyTrackerRoles = waist,left_foot,right_foot   (default)
// or "all". The default stays minimal because FBT mods auto-assign roles
// by pose height and a full skeleton of trackers would confuse them.

struct OCUTrackerRoleDef {
	const char* iniName;   // name used in the bodyTrackerRoles ini list
	const char* xrPath;    // HTCX role pose input path
	const char* serial;    // stable serial exposed to the game
	const char* actionName;    // OpenXR action name (lowercase, dashes)
	const char* localizedName; // OpenXR localized action name
	uint32_t minimumExtensionVersion = 1;
};

inline constexpr OCUTrackerRoleDef OCU_TRACKER_ROLES[] = {
	{ "waist", "/user/vive_tracker_htcx/role/waist/input/grip/pose", "OCU-WAIST", "body-tracker-waist", "Body Tracker: Waist" },
	{ "left_foot", "/user/vive_tracker_htcx/role/left_foot/input/grip/pose", "OCU-LFOOT", "body-tracker-left-foot", "Body Tracker: Left Foot" },
	{ "right_foot", "/user/vive_tracker_htcx/role/right_foot/input/grip/pose", "OCU-RFOOT", "body-tracker-right-foot", "Body Tracker: Right Foot" },
	{ "chest", "/user/vive_tracker_htcx/role/chest/input/grip/pose", "OCU-CHEST", "body-tracker-chest", "Body Tracker: Chest" },
	{ "left_knee", "/user/vive_tracker_htcx/role/left_knee/input/grip/pose", "OCU-LKNEE", "body-tracker-left-knee", "Body Tracker: Left Knee" },
	{ "right_knee", "/user/vive_tracker_htcx/role/right_knee/input/grip/pose", "OCU-RKNEE", "body-tracker-right-knee", "Body Tracker: Right Knee" },
	{ "left_elbow", "/user/vive_tracker_htcx/role/left_elbow/input/grip/pose", "OCU-LELBOW", "body-tracker-left-elbow", "Body Tracker: Left Elbow" },
	{ "right_elbow", "/user/vive_tracker_htcx/role/right_elbow/input/grip/pose", "OCU-RELBOW", "body-tracker-right-elbow", "Body Tracker: Right Elbow" },
	{ "left_shoulder", "/user/vive_tracker_htcx/role/left_shoulder/input/grip/pose", "OCU-LSHLDR", "body-tracker-left-shoulder", "Body Tracker: Left Shoulder" },
	{ "right_shoulder", "/user/vive_tracker_htcx/role/right_shoulder/input/grip/pose", "OCU-RSHLDR", "body-tracker-right-shoulder", "Body Tracker: Right Shoulder" },
	{ "left_wrist", "/user/vive_tracker_htcx/role/left_wrist/input/grip/pose", "OCU-LWRIST", "body-tracker-left-wrist", "Body Tracker: Left Wrist", 3 },
	{ "right_wrist", "/user/vive_tracker_htcx/role/right_wrist/input/grip/pose", "OCU-RWRIST", "body-tracker-right-wrist", "Body Tracker: Right Wrist", 3 },
	{ "left_ankle", "/user/vive_tracker_htcx/role/left_ankle/input/grip/pose", "OCU-LANKLE", "body-tracker-left-ankle", "Body Tracker: Left Ankle", 3 },
	{ "right_ankle", "/user/vive_tracker_htcx/role/right_ankle/input/grip/pose", "OCU-RANKLE", "body-tracker-right-ankle", "Body Tracker: Right Ankle", 3 },
};

inline constexpr int OCU_TRACKER_ROLE_COUNT = sizeof(OCU_TRACKER_ROLES) / sizeof(OCU_TRACKER_ROLES[0]);

inline bool OcuTrackerRoleRequested(std::string_view roleList, int role)
{
	if (role < 0 || role >= OCU_TRACKER_ROLE_COUNT)
		return false;
	auto trim = [](std::string_view value) {
		auto whitespace = [](char c) { return c == ' ' || (c >= '\t' && c <= '\r'); };
		while (!value.empty() && whitespace(value.front())) value.remove_prefix(1);
		while (!value.empty() && whitespace(value.back())) value.remove_suffix(1);
		return value;
	};
	auto matches = [](std::string_view value, std::string_view lowercaseName) {
		if (value.size() != lowercaseName.size()) return false;
		for (size_t i = 0; i < value.size(); ++i) {
			const char c = value[i] >= 'A' && value[i] <= 'Z'
			    ? static_cast<char>(value[i] + ('a' - 'A')) : value[i];
			if (c != lowercaseName[i]) return false;
		}
		return true;
	};
	roleList = trim(roleList);
	if (matches(roleList, "all"))
		return true;
	const std::string_view name = OCU_TRACKER_ROLES[role].iniName;
	while (!roleList.empty()) {
		const auto end = roleList.find(',');
		if (matches(trim(roleList.substr(0, end)), name))
			return true;
		if (end == std::string_view::npos)
			break;
		roleList.remove_prefix(end + 1);
	}
	return false;
}

inline bool OcuTrackerRoleEnabled(std::string_view roleList, int role, uint32_t extensionVersion)
{
	return OcuTrackerRoleRequested(roleList, role)
	    && extensionVersion >= OCU_TRACKER_ROLES[role].minimumExtensionVersion;
}
