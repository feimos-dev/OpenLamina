// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

#include "Misc/SteamVRPaths.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {
// A fresh folder under the system's temporary one, removed again at the end
struct TempDir {
	fs::path path;
	TempDir()
	{
		auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		path = fs::temp_directory_path() / ("oc_steamvr_paths_" + std::to_string(stamp));
		fs::create_directories(path);
	}
	~TempDir()
	{
		std::error_code ec;
		fs::remove_all(path, ec);
	}
};

std::string JsonString(const fs::path& p)
{
	// The paths file is JSON: backslashes escaped
	std::u8string u8 = p.u8string();
	std::string s(u8.begin(), u8.end());
	std::string out;
	for (char c : s) {
		if (c == '\\')
			out += "\\\\";
		else
			out += c;
	}
	return out;
}
} // namespace

TEST_CASE("SteamVRPaths_from_paths_file")
{
	TempDir tmp;
	fs::path steamvr = tmp.path / "SteamVR";
	fs::create_directories(steamvr / "bin");
	fs::path pathsFile = tmp.path / "openvrpaths.vrpath";
	std::ofstream(pathsFile) << "{ \"runtime\": [ \"" << JsonString(tmp.path / "Other") << "\", \"" << JsonString(steamvr) << "\" ] }";
	fs::path noSteam = tmp.path / "no steam";

	auto found = FindSteamVRInstall(&pathsFile, &noSteam);
	REQUIRE(found.has_value());
	CHECK(fs::equivalent(*found, steamvr));
}

TEST_CASE("SteamVRPaths_none")
{
	TempDir tmp;
	// A SteamVR folder without bin/ isn't an install
	fs::create_directories(tmp.path / "SteamVR");
	fs::path pathsFile = tmp.path / "openvrpaths.vrpath";
	std::ofstream(pathsFile) << "{ \"runtime\": [ \"" << JsonString(tmp.path / "SteamVR") << "\" ] }";
	fs::path noSteam = tmp.path / "no steam";
	CHECK_FALSE(FindSteamVRInstall(&pathsFile, &noSteam).has_value());

	// No paths file at all
	fs::path missing = tmp.path / "missing.vrpath";
	CHECK_FALSE(FindSteamVRInstall(&missing, &noSteam).has_value());
}

TEST_CASE("SteamVRPaths_from_steam_library")
{
	// No paths file entry: Steam's library folders are looked through
	TempDir tmp;
	fs::path steam = tmp.path / "Steam";
	fs::path library = tmp.path / "Library";
	fs::create_directories(steam / "steamapps");
	fs::create_directories(library / "steamapps" / "common" / "SteamVR" / "bin");
	std::ofstream(steam / "steamapps" / "libraryfolders.vdf")
	    << "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"" << JsonString(steam) << "\"\n\t}\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\""
	    << JsonString(library) << "\"\n\t}\n}\n";
	fs::path missing = tmp.path / "missing.vrpath";

	auto found = FindSteamVRInstall(&missing, &steam);
	REQUIRE(found.has_value());
	CHECK(fs::equivalent(*found, library / "steamapps" / "common" / "SteamVR"));
}

TEST_CASE("SteamVRPaths_non_ansi_entry")
{
	// Another runtime in a folder whose name the ANSI code page can't spell comes first: it's passed over, not thrown on
	TempDir tmp;
	fs::path other = tmp.path / fs::path(u8"テスト");
	fs::path steamvr = tmp.path / "SteamVR";
	fs::create_directories(other / "bin");
	fs::create_directories(steamvr / "bin");
	fs::path pathsFile = tmp.path / "openvrpaths.vrpath";
	std::ofstream(pathsFile) << "{ \"runtime\": [ \"" << JsonString(other) << "\", \"" << JsonString(steamvr) << "\" ] }";
	fs::path noSteam = tmp.path / "no steam";

	std::optional<fs::path> found;
	CHECK_NOTHROW(found = FindSteamVRInstall(&pathsFile, &noSteam));
	REQUIRE(found.has_value());
	CHECK(fs::equivalent(*found, steamvr));
}

TEST_CASE("SteamVRPaths_bad_utf8_library")
{
	// A library path that isn't UTF-8 (libraryfolders.vdf is read as bytes) is passed over, not thrown on
	TempDir tmp;
	fs::path steam = tmp.path / "Steam";
	fs::create_directories(steam / "steamapps" / "common" / "SteamVR" / "bin");
	std::ofstream(steam / "steamapps" / "libraryfolders.vdf", std::ios::binary)
	    << "\"libraryfolders\"\n{\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"C:\\\xff\xfe\xc3\"\n\t}\n}\n";
	fs::path missing = tmp.path / "missing.vrpath";

	std::optional<fs::path> found;
	CHECK_NOTHROW(found = FindSteamVRInstall(&missing, &steam));
	REQUIRE(found.has_value());
	CHECK(fs::equivalent(*found, steam / "steamapps" / "common" / "SteamVR"));
}

TEST_CASE("SteamVRPaths_config_from_paths_file")
{
	// The paths file's config entry wins over Steam's own config folder; a first entry that isn't a folder is passed over
	TempDir tmp;
	fs::path steam = tmp.path / "Steam";
	fs::create_directories(steam / "config");
	fs::path config = tmp.path / "elsewhere" / "config";
	fs::create_directories(config);
	fs::path pathsFile = tmp.path / "openvrpaths.vrpath";
	std::ofstream(pathsFile) << "{ \"config\": [ \"" << JsonString(tmp.path / "missing") << "\", \"" << JsonString(config) << "\" ] }";

	auto found = FindSteamConfigDir(&pathsFile, &steam);
	REQUIRE(found.has_value());
	CHECK(fs::equivalent(*found, config));

	// Without a config entry, Steam's
	fs::path missing = tmp.path / "missing.vrpath";
	found = FindSteamConfigDir(&missing, &steam);
	REQUIRE(found.has_value());
	CHECK(fs::equivalent(*found, steam / "config"));

	fs::path noSteam = tmp.path / "no steam";
	CHECK_FALSE(FindSteamConfigDir(&missing, &noSteam).has_value());
}

#ifndef _WIN32
TEST_CASE("SteamVRPaths_flatpak_steam")
{
	TempDir tmp;
	fs::path steam = tmp.path / ".var/app/com.valvesoftware.Steam/.local/share/Steam";
	fs::create_directories(steam / "config");
	fs::create_directories(steam / "steamapps/common/SteamVR/bin");
	fs::path missing = tmp.path / "missing.vrpath";

	const char* oldHome = getenv("HOME");
	std::string saved = oldHome ? oldHome : "";
	setenv("HOME", tmp.path.c_str(), 1);
	auto install = FindSteamVRInstall(&missing);
	auto config = FindSteamConfigDir(&missing);
	if (oldHome)
		setenv("HOME", saved.c_str(), 1);
	else
		unsetenv("HOME");

	REQUIRE(install.has_value());
	CHECK(fs::equivalent(*install, steam / "steamapps/common/SteamVR"));
	REQUIRE(config.has_value());
	CHECK(fs::equivalent(*config, steam / "config"));
}
#endif
