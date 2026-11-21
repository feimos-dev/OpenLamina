// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

#include "Misc/Config.h"
#include "Misc/Input/IndexControllerInteractionProfile.h"
#include "logging.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

TEST_CASE("Index profile has its A button")
{
	IndexControllerInteractionProfile p;
	CHECK(p.IsInputPathValid("/user/hand/right/input/a/click"));
}

TEST_CASE("Config defaults")
{
	Config c;
	CHECK(c.SupersampleRatio() == 1.0f);
	CHECK_FALSE(c.StopOnSoftAbort());
}

static std::vector<std::string> sink_lines;
static void test_sink(const char* line) { sink_lines.emplace_back(line); }

TEST_CASE("Log sink sees lines")
{
	sink_lines.clear();
	oovr_log_set_test_sink(test_sink);
	OOVR_LOG("hello-sink");
	oovr_log_set_test_sink(nullptr);

	bool seen = false;
	for (const std::string& l : sink_lines)
		seen = seen || l.find("hello-sink") != std::string::npos;
	CHECK(seen);
}

namespace {
// An openlamina.ini in a fresh working directory, for the Config built while it is there
struct IniInDir {
	std::filesystem::path old = std::filesystem::current_path();
	std::filesystem::path dir;

	explicit IniInDir(const std::string& text)
	{
		dir = std::filesystem::temp_directory_path() / ("oc_ini_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		std::filesystem::create_directories(dir);
		std::ofstream(dir / "openlamina.ini") << text;
		std::filesystem::current_path(dir);
		sink_lines.clear();
	}
	~IniInDir()
	{
		std::filesystem::current_path(old);
		std::error_code ec;
		std::filesystem::remove_all(dir, ec);
	}
	bool Logged(const char* part) const
	{
		for (const std::string& l : sink_lines)
			if (l.find(part) != std::string::npos)
				return true;
		return false;
	}
};
} // namespace

TEST_CASE("Config is read at the first question, not when it's made")
{
	// Made before the program is in the folder with its openlamina.ini, as the library's global is
	Config c;
	IniInDir ini("supersampleRatio=1.5\nlaserMouse=false\n");
	CHECK(c.SupersampleRatio() == 1.5f);
	CHECK_FALSE(c.LaserMouse());
	// Read once: a file changed later isn't read again
	std::ofstream(ini.dir / "openlamina.ini") << "supersampleRatio=2\n";
	CHECK(c.SupersampleRatio() == 1.5f);
}

#ifndef _WIN32
// On Linux the user's config folder ($XDG_CONFIG_HOME/openlamina) is read before the working directory, which overrides
// it key by key
TEST_CASE("Config user file overridden by the working directory's, key by key")
{
	oovr_log_set_test_sink(test_sink);
	IniInDir ini("supersampleRatio=2\n");
	std::filesystem::path config = ini.dir / "xdg";
	std::filesystem::create_directories(config / "openlamina");
	std::ofstream(config / "openlamina" / "openlamina.ini") << "supersampleRatio=1.5\nlaserMouse=false\n";
	const char* oldXdg = getenv("XDG_CONFIG_HOME");
	std::string saved = oldXdg ? oldXdg : "";
	setenv("XDG_CONFIG_HOME", config.c_str(), 1);
	{
		Config c;
		CHECK(c.SupersampleRatio() == 2.0f);
		CHECK_FALSE(c.LaserMouse());
		CHECK(ini.Logged("Checking for user config file"));
		CHECK(ini.Logged("2 config files read"));
	}
	if (oldXdg)
		setenv("XDG_CONFIG_HOME", saved.c_str(), 1);
	else
		unsetenv("XDG_CONFIG_HOME");
	oovr_log_set_test_sink(nullptr);
}
#endif

TEST_CASE("Config scales must be numbers above 0")
{
	oovr_log_set_test_sink(test_sink);
	{
		IniInDir ini("supersampleRatio=\nhiddenMeshVerticalScale=0\n");
		Config c;
		CHECK(c.SupersampleRatio() == 1.0f);
		CHECK(c.HiddenMeshVerticalScale() == 1.0f);
		CHECK(ini.Logged("supersampleRatio on line 1"));
		CHECK(ini.Logged("hiddenMeshVerticalScale on line 2"));
	}
	{
		IniInDir ini("supersampleRatio=-1\nhiddenMeshVerticalScale=abc\n");
		Config c;
		CHECK(c.SupersampleRatio() == 1.0f);
		CHECK(c.HiddenMeshVerticalScale() == 1.0f);
	}
	{
		IniInDir ini("supersampleRatio=1.5\nhiddenMeshVerticalScale=0.8\n");
		Config c;
		CHECK(c.SupersampleRatio() == 1.5f);
		CHECK(c.HiddenMeshVerticalScale() == doctest::Approx(0.8f));
	}
	oovr_log_set_test_sink(nullptr);
}

TEST_CASE("Config reads keys outside any section or in default, and refuses other sections")
{
	oovr_log_set_test_sink(test_sink);
	{
		IniInDir ini("supersampleRatio=1.2\n[default]\nhaptics=off\n[other]\nsupersampleRatio=2\nkeyboard=off\n");
		Config c;
		CHECK(c.SupersampleRatio() == doctest::Approx(1.2f));
		CHECK_FALSE(c.Haptics());
		CHECK(c.Keyboard());
		CHECK(ini.Logged("in section [other] ignored"));
	}
	oovr_log_set_test_sink(nullptr);
}
