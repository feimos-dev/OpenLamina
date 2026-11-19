// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

#include "Reimpl/BaseControlPanel.h"
#include "Reimpl/BaseResources.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {
// A SteamVR install of our own files, named by OC_TEST_STEAMVR_DIR while it lives
struct FakeSteamVR {
	fs::path path;
	FakeSteamVR()
	{
		auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		path = fs::temp_directory_path() / ("oc_resources_" + std::to_string(stamp));
		fs::create_directories(path / "resources" / "images");
		std::ofstream(path / "resources" / "images" / "a.txt", std::ios::binary) << "abc";
#ifdef _WIN32
		_putenv_s("OC_TEST_STEAMVR_DIR", path.string().c_str());
#else
		setenv("OC_TEST_STEAMVR_DIR", path.c_str(), 1);
#endif
	}
	~FakeSteamVR()
	{
#ifdef _WIN32
		_putenv_s("OC_TEST_STEAMVR_DIR", "");
#else
		unsetenv("OC_TEST_STEAMVR_DIR");
#endif
		std::error_code ec;
		fs::remove_all(path, ec);
	}
};
} // namespace

TEST_CASE("Resource_paths_stay_inside_the_resources_folder")
{
	FakeSteamVR steamvr;
	BaseResources r;
	char buf[1024];

	const uint32_t size = r.GetResourceFullPath("a.txt", "images", buf, sizeof(buf));
	CHECK(size == strlen(buf) + 1);
	CHECK(std::string(buf).find("a.txt") != std::string::npos);

	// The folder itself, as an empty name
	CHECK(r.GetResourceFullPath("", "", buf, sizeof(buf)) > 0);

	CHECK(r.GetResourceFullPath("../../outside", "images", buf, sizeof(buf)) == 0);
	CHECK(r.GetResourceFullPath("a.txt", "../..", buf, sizeof(buf)) == 0);
	CHECK(r.GetResourceFullPath("/etc/passwd", "images", buf, sizeof(buf)) == 0);
	CHECK(buf[0] == '\0');

	CHECK(r.LoadSharedResource("images/a.txt", buf, sizeof(buf)) == 3);
	CHECK(r.LoadSharedResource("../resources/images/a.txt", buf, sizeof(buf)) == 0);
}

TEST_CASE("Control_panel_reads_the_same_shared_resources")
{
	FakeSteamVR steamvr;
	BaseControlPanel panel;
	char buf[16] = {};
	CHECK(panel.LoadSharedResource("images/a.txt", buf, sizeof(buf)) == 3);
	CHECK(std::string(buf, 3) == "abc");
	CHECK(panel.LoadSharedResource("images/missing.txt", buf, sizeof(buf)) == 0);
}
