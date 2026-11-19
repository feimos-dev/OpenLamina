#pragma once

// Where SteamVR and Steam are, for the calls that answer with SteamVR's own files (IVRResources)

#include <filesystem>
#include <optional>

/**
 * SteamVR's install folder: the OpenVR paths file's runtime entry named SteamVR (as BaseClientCore reads it), else
 * <library>/steamapps/common/SteamVR in one of Steam's library folders (libraryfolders.vdf). It must contain bin/.
 * The overrides replace the paths file and Steam's folder (for tests). Without them, the environment variable
 * OC_TEST_STEAMVR_DIR (tests only) is the answer when set, and OC_TEST_NO_STEAMVR makes it none.
 */
std::optional<std::filesystem::path> FindSteamVRInstall(const std::filesystem::path* pathsFileOverride = nullptr,
    const std::filesystem::path* steamDirOverride = nullptr);

/**
 * SteamVR's config folder: the OpenVR paths file's first config entry that is a folder, else Steam's config folder.
 * Windows: HKCU\Software\Valve\Steam\SteamPath + \config; Linux: ~/.steam/steam/config, or where the Flatpak and
 * Snap packages keep Steam. The overrides replace the paths file and Steam's folder (for tests).
 */
std::optional<std::filesystem::path> FindSteamConfigDir(const std::filesystem::path* pathsFileOverride = nullptr,
    const std::filesystem::path* steamDirOverride = nullptr);
