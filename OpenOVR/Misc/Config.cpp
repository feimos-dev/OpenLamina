#include "stdafx.h"

#include "Config.h"
#include "Numbers.h"
#include "ini.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

using vr::HmdColor_t;

Config oovr_global_configuration;

// A bad value in openlamina.ini: logged as a warning, and the option keeps its default (parse_* throw this to
// ini_handler, which catches it; nothing leaves this file)
struct BadConfigValue {
	string message;
};
#define BAD_VALUE(msg) throw BadConfigValue{ msg }

static string str_tolower(std::string val)
{
	transform(val.begin(), val.end(), val.begin(), ::tolower);
	return val;
}

unsigned char hexval(unsigned char c)
{
	if ('0' <= c && c <= '9')
		return c - '0';
	else if ('a' <= c && c <= 'f')
		return c - 'a' + 10;
	else
		return 0;
}

static bool parse_bool(string orig, string name, int line)
{
	string val = str_tolower(orig);

	if (val == "true" || val == "on" || val == "enabled") {
		OOVR_LOGF("Setting config param %s to true", name.c_str());
		return true;
	}
	if (val == "false" || val == "off" || val == "disabled") {
		OOVR_LOGF("Setting config param %s to false", name.c_str());
		return false;
	}

	string err = "Value " + orig + " for " + name + " on line "
	    + to_string(line) + " is not a boolean - true/on/enabled/false/off/disabled";
	BAD_VALUE(err);
}

static HmdColor_t parse_HmdColor_t(string orig, string name, int line)
{
	string val = str_tolower(orig);

	if (val.length() != 7 || val[0] != '#') {
		goto invalid;
	}

	for (size_t i = 1; i < val.length(); i++) {
		char c = val[i];
		if ((c < '0' || c > '9') && (c < 'a' || c > 'f'))
			goto invalid;
	}

	HmdColor_t c;
	c.a = 255; // Full alpha

	c.r = ((hexval(val[1]) << 4) + hexval(val[2])) / 255.0f;
	c.g = ((hexval(val[3]) << 4) + hexval(val[4])) / 255.0f;
	c.b = ((hexval(val[5]) << 4) + hexval(val[6])) / 255.0f;

	OOVR_LOGF("Setting config param %s to %f %f %f", name.c_str(), c.r, c.g, c.b);
	return c;

invalid:

	string err = "Value " + orig + " for " + name + " on line "
	    + to_string(line) + " is not a hex (CSS) colour code";
	BAD_VALUE(err);
}

static float parse_float(string orig, string name, int line)
{
	// The whole value a number, in any locale; the options that take one are scales, so it has to be above 0
	size_t used = 0;
	std::optional<float> parsed = Numbers::ParseFloat(orig, &used);

	if (!parsed || used != orig.length()) {
		string err = "Value " + orig + " for " + name + " on line "
		    + to_string(line) + " is not a decimal number (eg 12.34)";
		BAD_VALUE(err);
	}

	float result = *parsed;
	if (!std::isfinite(result) || result <= 0.0f) {
		string err = "Value " + orig + " for " + name + " on line "
		    + to_string(line) + " is not above 0";
		BAD_VALUE(err);
	}

	OOVR_LOGF("Setting config param %s to %f", name.c_str(), result);
	return result;
}

// A whole number of 0 or more
static uint32_t parse_uint32_t(string orig, string name, int line)
{
	size_t used = 0;
	unsigned long long value = 0;
	try {
		value = std::stoull(orig, &used);
	} catch (const std::exception&) {
		used = 0;
	}
	if (orig.empty() || used != orig.length() || orig[0] == '-' || value > 0xffffffffULL) {
		BAD_VALUE("Value " + orig + " for " + name + " on line " + to_string(line) + " is not a whole number of 0 or more");
	}
	OOVR_LOGF("Setting config param %s to %llu", name.c_str(), value);
	return (uint32_t)value;
}

// dashboardButton: system, system_long, menu, menu_long or off
static string parse_dashboard(string orig, string name, int line)
{
	string val = str_tolower(orig);
	if (val != "system" && val != "system_long" && val != "menu_long" && val != "menu" && val != "off") {
		BAD_VALUE("Value " + orig + " for " + name + " on line " + to_string(line) + " is none of system, system_long, menu, menu_long and off");
	}
	OOVR_LOGF("Setting config param %s to %s", name.c_str(), val.c_str());
	return val;
}

static string parse_string(string orig, string name, int line)
{
	OOVR_LOGF("Setting config param %s to %s", name.c_str(), orig.c_str());
	return orig;
}

// keyboardLayout: us or uk
static string parse_layout(string orig, string name, int line)
{
	string val = str_tolower(orig);
	if (val != "us" && val != "uk") {
		BAD_VALUE("Value " + orig + " for " + name + " on line " + to_string(line) + " is neither us nor uk");
	}
	OOVR_LOGF("Setting config param %s to %s", name.c_str(), val.c_str());
	return val;
}

int Config::ini_handler(void* user, const char* pSection,
    const char* pName, const char* pValue,
    int lineno)
{

	string section = pSection;
	string name = pName;
	string value = pValue;

	Config* cfg = (Config*)user;

#define CFGOPT(type, vname)                                                    \
	if (name == #vname) {                                                      \
		try {                                                                  \
			cfg->vname = parse_##type(value, name, lineno);                    \
		} catch (const BadConfigValue& bad) {                                  \
			OOVR_LOGF("Warning: %s; keeping the default", bad.message.c_str()); \
		}                                                                      \
		return true;                                                           \
	}

	if (section == "" || section == "default") {
		CFGOPT(bool, renderCustomHands);
		CFGOPT(HmdColor_t, handColour);
		CFGOPT(float, supersampleRatio);
		CFGOPT(bool, haptics);
		CFGOPT(bool, eyeTracking);
		CFGOPT(bool, admitUnknownProps);
		CFGOPT(bool, logGetTrackedProperty);
		CFGOPT(bool, stopOnSoftAbort);
		CFGOPT(bool, enableHiddenMeshFix);
		CFGOPT(bool, invertUsingShaders);
		CFGOPT(bool, initUsingVulkan);
		CFGOPT(bool, useSubmittedPose);
		CFGOPT(bool, submitDepth);
		CFGOPT(bool, ownFrames);
		CFGOPT(bool, mirrorWindow);
		CFGOPT(bool, keyboard);
		CFGOPT(bool, laserMouse);
		CFGOPT(bool, steamvrRenderModels);
		CFGOPT(bool, helperApps);
		CFGOPT(uint32_t, overlayLayersPlacement);
		CFGOPT(dashboard, dashboardButton);
		CFGOPT(bool, drawBounds);
		CFGOPT(bool, sessionEvents);
		CFGOPT(bool, applyZeroPoses);
		CFGOPT(float, hiddenMeshVerticalScale);
		CFGOPT(bool, logAllOpenVRCalls);
		CFGOPT(string, bindingEditor);
		// Read and ignored, so an ini file that sets it still loads
		if (name == "inputEvaluator") {
			OOVR_LOGF("Config key inputEvaluator (%s, line %d) no longer does anything: binding files are always evaluated by OpenLamina", value.c_str(), lineno);
			return true;
		}
		CFGOPT(layout, keyboardLayout);

		// Accepted and ignored: Direct3D 10 textures are told apart from Direct3D 11 ones by their interface
		if (name == "dx10Mode") {
			OOVR_LOG("Config file key 'dx10Mode' has no effect and is ignored");
			return true;
		}
	} else {
		OOVR_LOGF("Config file key '%s' in section [%s] ignored (line %d): only keys outside any section, or in [default], are read",
		    name.c_str(), section.c_str(), lineno);
		return true;
	}

#undef CFGOPT

	OOVR_LOGF("Unknown config file key '%s' ignored (line %d)", name.c_str(), lineno);
	return true;
}

static int wini_parse(const std::filesystem::path& filename, ini_handler handler, void* user)
{
	const std::u8string utf8 = filename.u8string();
	const std::string name(utf8.begin(), utf8.end());
#ifdef _WIN32
	FILE* file = _wfopen(filename.c_str(), L"r");
#else
	FILE* file = fopen(filename.c_str(), "r");
#endif
	if (!file) {
		OOVR_LOGF("No config file found at %s", name.c_str());
		return -1;
	}

	OOVR_LOGF("Reading config file at %s", name.c_str());

	int error = ini_parse_file(file, handler, user);
	fclose(file);
	return error;
}

// The config file in a folder: openlamina.ini, else the opencomposite.ini of the project OpenLamina was forked from,
// so a file a user already has keeps working. If both are there, only openlamina.ini is read.
static int wini_parse_in(const std::filesystem::path& dir, ini_handler handler, void* user)
{
	int err = wini_parse(dir / "openlamina.ini", handler, user);
	if (err == -1)
		err = wini_parse(dir / "opencomposite.ini", handler, user);
	return err;
}

// The DLL's own module handle, for the folder it's in
#ifdef _WIN32
EXTERN_C IMAGE_DOS_HEADER __ImageBase;
#define HINST_THISCOMPONENT ((HINSTANCE) & __ImageBase)
#endif

Config::Config()
{
	// Initialise using Vulkan if D3D11 is unavailable
#if !defined(SUPPORT_DX11)
	initUsingVulkan = true;
#endif
}

const Config& Config::Loaded() const
{
	// The options only change while the files are read, once
	std::call_once(loaded, [this] { const_cast<Config*>(this)->Read(); });
	return *this;
}

void Config::Read()
{
	// If we're on Windows, look for a config file next to the DLL
	// If we're on Linux, skip that and just check the working directory.
	int err = -1;
#ifdef _WIN32
	wchar_t buffer[MAX_PATH];
	DWORD len = GetModuleFileNameW(HINST_THISCOMPONENT, buffer, MAX_PATH);
	if (len) {
		OOVR_LOG("Checking for global config file...");
		err = wini_parse_in(std::filesystem::path(std::wstring(buffer, len)).parent_path(), ini_handler, this);
	}
#endif

	if (err > 0) {
		// err is the line number of the first line ini_parse couldn't read; it read the rest
		OOVR_LOGF("Warning: the config file can't be read on line %d; that line is ignored", err);
	}

	{
		// The working directory's file overrides, key by key, what the first one set
		std::error_code ec;
		std::filesystem::path workingDir = std::filesystem::current_path(ec);
		if (!ec) {
			OOVR_LOG("Checking for app specific config file...");
			err = wini_parse_in(workingDir, ini_handler, this);
		} else {
			OOVR_LOG("Couldn't get the working directory, so there's no app specific config file");
			err = -1;
		}
	}

	if (err == -1) {
		// Couldn't open file, no problen since the config file is optional and
		//  the defaults are set up as the default values for the variables
		return;
	} else if (err) {
		// err is the line number of the first line ini_parse couldn't read; it read the rest
		OOVR_LOGF("Warning: the config file can't be read on line %d; that line is ignored", err);
	}

	// Everything should have been set up by ini_handler
}

Config::~Config()
{
}
