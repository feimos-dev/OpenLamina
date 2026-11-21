#include "stdafx.h"
#define BASE_IMPL
#include "BaseClientCore.h"
#include "steamvr_abi.h"

#include <fstream>

#include "json/json.h"

#include "Misc/JsonFile.h"
#include "Misc/SteamClient.h"

#ifdef _WIN32
#include <ShlObj.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

using namespace vr;

// UTF-8 to wchar_t (UTF-32 on Linux, UTF-16 on Windows); bytes that aren't valid UTF-8 become U+FFFD
static wstring Utf8ToWide(const string& in)
{
	wstring out;
	for (size_t i = 0; i < in.size();) {
		const unsigned char c = (unsigned char)in[i];
		uint32_t cp = 0xFFFD;
		size_t len = 1;
		if (c < 0x80) {
			cp = c;
		} else if (c >= 0xC2 && c < 0xE0) {
			len = 2;
			cp = c & 0x1F;
		} else if (c >= 0xE0 && c < 0xF0) {
			len = 3;
			cp = c & 0x0F;
		} else if (c >= 0xF0 && c < 0xF5) {
			len = 4;
			cp = c & 0x07;
		}
		if (len > 1) {
			bool valid = i + len <= in.size();
			for (size_t j = 1; valid && j < len; j++) {
				const unsigned char cc = (unsigned char)in[i + j];
				valid = (cc & 0xC0) == 0x80;
				cp = (cp << 6) | (cc & 0x3F);
			}
			const uint32_t minimum[] = { 0, 0, 0x80, 0x800, 0x10000 };
			if (!valid || cp < minimum[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) {
				cp = 0xFFFD;
				len = 1;
			}
		}
		i += len;
		if (sizeof(wchar_t) == 2 && cp > 0xFFFF) {
			cp -= 0x10000;
			out.push_back((wchar_t)(0xD800 + (cp >> 10)));
			out.push_back((wchar_t)(0xDC00 + (cp & 0x3FF)));
		} else {
			out.push_back((wchar_t)cp);
		}
	}
	return out;
}

// applist.json, apps-config.json and openvrpaths.vrpath are shared with other programs: a broken or half-written
// one reads as empty instead of ending the program, and writes replace the file in one step (Misc/JsonFile.h)
static bool ReadJson(const wstring& path, Json::Value& result)
{
	return oc_read_json_file(std::filesystem::path(path), result);
}

#ifdef _WIN32
// False if applist.json is there but couldn't be read: writing it back would delete every other program's entry
static bool ReadAppList(const wstring& path, Json::Value& root)
{
	std::error_code ec;
	if (oc_read_json_file(std::filesystem::path(path), root) || (!std::filesystem::exists(path, ec) && !ec))
		return true;
	OOVR_LOG_ONCE("applist.json couldn't be read: leaving it as it is");
	return false;
}

static void WriteJson(const wstring& path, const Json::Value& value)
{
	// The game goes on if it fails: the list only records which programs loaded OpenComposite
	oc_write_json_file(std::filesystem::path(path), value);
}
#endif

enum Runtime {
	Default = 0,
	OpenComposite = 1,
	SteamVR = 2,
};

// An integer member of an object, or Default if the value isn't an object or the member isn't an integer: the file
// is written by other programs, and a value of another type reads as unset
static Runtime RuntimeMember(const Json::Value& value, const char* name)
{
	if (!value.isObject())
		return Runtime::Default;
	const Json::Value& member = value[name];
	return member.isInt() ? (Runtime)member.asInt() : Runtime::Default;
}

int BaseClientCore::ConfiguredRuntime(const Json::Value& config, const std::string& appPath)
{
	if (!config.isObject())
		return Runtime::OpenComposite;

	// If a runtime has been set for this program in the GUI, use that; if not, the default runtime if one is set
	Runtime selected = RuntimeMember(config[appPath], "runtime");
	if (selected == Runtime::Default)
		selected = RuntimeMember(config["***GlobalConfig"], "default_runtime");

	// Finally, default to using OpenComposite
	if (selected == Runtime::Default)
		selected = Runtime::OpenComposite;
	return selected;
}

bool BaseClientCore::CheckAppEnabled()
{
#ifndef _WIN32
	// No per-app runtime list on Linux: a game's runtime is chosen with VR_OVERRIDE in its launch options
	OOVR_LOG_ONCE("Choosing a runtime per game on Linux: set the game's launch options to VR_OVERRIDE=<dir> %command%, "
	              "where <dir> is the runtime's folder (OpenLamina's install, or SteamVR's)");
	return true;
#else
	wstring dlldir = GetDllDir();
	wstring listname = dlldir + L"applist.json";
	wstring configname = dlldir + L"apps-config.json";

	Json::Value tag(Json::ValueType::objectValue);
	// Put information about this app in the tag
	{
		SYSTEMTIME st;
		GetSystemTime(&st);
		FILETIME ft; // Contains a 64-bit value representing the number of 100-nanosecond intervals since January 1, 1601 (UTC).
		SystemTimeToFileTime(&st, &ft);
		ULARGE_INTEGER timestamp;
		timestamp.HighPart = ft.dwHighDateTime;
		timestamp.LowPart = ft.dwLowDateTime;

		// ms is now the number of milliseconds January 1, 1601 (UTC).
		uint64_t ms = timestamp.QuadPart / 10000;

		// Move to the UNIX epoch
		ms -= 11644473600000LL;

		tag["timestamp"] = Json::Value(ms);
	}

	string apppath = GetAppPath();
	Json::Value root;
	if (ReadAppList(listname, root)) {
		root[apppath] = tag;
		WriteJson(listname, root);
	}

	// Check what runtime the app should use, as set in the config menu
	Json::Value config;
	ReadJson(configname, config);
	Runtime selected = (Runtime)ConfiguredRuntime(config, apppath);

	// The Steam client is never sent to SteamVR: sending a program there sets VR_OVERRIDE in its environment, and
	// every game the Steam client starts inherits that and loads SteamVR's client instead of OpenLamina
	if (selected == Runtime::SteamVR && oc_is_steam_client_program()) {
		OOVR_LOG_ONCE("Not sending the Steam client to SteamVR: the programs it starts would follow it");
		selected = Runtime::OpenComposite;
	}

	return selected == Runtime::OpenComposite;
#endif
}

template <typename str_t>
static void TrimPath(str_t& path)
{
	if (!path.empty() && (path.back() == '\\' || path.back() == '/')) {
		path.erase(path.end() - 1);
	}
}

template <typename str_t>
bool EndsWith(const str_t& a, const str_t& b)
{
	if (b.size() > a.size())
		return false;
	return std::equal(a.begin() + a.size() - b.size(), a.end(), b.begin());
}

// Derived from OpenVR
static wstring GetAppSettingsPath()
{
	wstring path;
#if defined(WIN32)
	WCHAR rwchPath[MAX_PATH];

	if (!SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, NULL, 0, rwchPath))) {
		// The paths file then can't be found, which its readers handle
		OOVR_LOG("Failed to find the appdata directory");
		return {};
	}

	path = rwchPath;
#elif defined(OSX)
#error "Unsupported platform"
#elif defined(__linux__)
	// As defined by XDG Base Directory Specification
	// https://specifications.freedesktop.org/basedir-spec/basedir-spec-latest.html

	const char* pchHome = getenv("XDG_CONFIG_HOME");
	if ((pchHome != NULL) && (pchHome[0] != '\0')) {
		return Utf8ToWide(pchHome);
	}

	//
	// XDG_CONFIG_HOME is not defined, use ~/.config instead
	//
	pchHome = getenv("HOME");
	if (pchHome == NULL) {
		return L"";
	}

	wstring homeDir = Utf8ToWide(pchHome);
	TrimPath(homeDir);
	path = homeDir + L"/.config";
#else
#error "Unsupported platform"
#endif

	TrimPath(path);

	return path;
}

static wstring GetOpenVRConfigPath()
{
	wstring sConfigPath = GetAppSettingsPath();
	if (sConfigPath.empty())
		return {};

#if defined(_WIN32) || defined(__linux__)
	sConfigPath += L"/openvr";
#elif defined(OSX)
	sConfigPath += L"/.openvr";
#else
#error "Unsupported platform"
#endif
	return sConfigPath;
}

string BaseClientCore::GetAlternativeRuntimePath()
{
	wstring regPath = GetOpenVRConfigPath();
	if (regPath.empty()) {
		OOVR_LOG("Cannot start SteamVR: no config folder");
		return {};
	}

#if defined(_WIN32) || defined(__unix__)
	regPath += L"/openvrpaths.vrpath";
#else
#error "Unsupported platform"
#endif

	// "" (logged) if there's no SteamVR to chain to: PlatformGetAlternativeCoreFactory then gives up
	Json::Value root;
	if (!ReadJson(regPath, root)) {
		OOVR_LOG("Cannot start SteamVR: openvrpaths.vrpath couldn't be read");
		return {};
	}

	const Json::Value runtimes = root["runtime"];
	if (!runtimes.isArray()) {
		OOVR_LOG("Cannot start SteamVR: malformed runtime path");
		return {};
	}

	for (unsigned int i = 0; i < runtimes.size(); i++) {
		if (!runtimes[i].isString())
			continue;
		string path = runtimes[i].asString();
		TrimPath(path);

		if (EndsWith(path, string("\\SteamVR")) || EndsWith(path, string("/SteamVR")))
			return path;
	}

	OOVR_LOG("Could not find a usable SteamVR installation");
	return {};
}

EVRInitError BaseClientCore::Init(vr::EVRApplicationType eApplicationType, const char* pStartupInfo)
{
	EVRInitError err;
	VR_InitInternal2(&err, eApplicationType, pStartupInfo);
	return err;
}

void BaseClientCore::Cleanup()
{
	// Note that this object is not affected by the shutdown, as it is handled seperately
	//  from all the other interface objects and is only destroyed when the DLL is unloaded.
	VR_ShutdownInternal();
}

EVRInitError BaseClientCore::IsInterfaceVersionValid(const char* pchInterfaceVersion)
{
	return VR_IsInterfaceVersionValid(pchInterfaceVersion) ? VRInitError_None : VRInitError_Init_InvalidInterface;
}

void* BaseClientCore::GetGenericInterface(const char* pchNameAndVersion, EVRInitError* peError)
{
	return VR_GetGenericInterface(pchNameAndVersion, peError);
}

bool BaseClientCore::BIsHmdPresent()
{
	return VR_IsHmdPresent();
}

const char* BaseClientCore::GetEnglishStringForHmdError(vr::EVRInitError eError)
{
	return VR_GetVRInitErrorAsEnglishDescription(eError);
}

const char* BaseClientCore::GetIDForVRInitError(vr::EVRInitError eError)
{
	return VR_GetVRInitErrorAsSymbol(eError);
}

std::string BaseClientCore::GetAppPath()
{
#ifndef _WIN32
	char appExe[4096];
	ssize_t len = readlink("/proc/self/exe", appExe, sizeof(appExe) - 1);
	return len > 0 ? string(appExe, (size_t)len) : string();
#else
	char appExe[MAX_PATH];
	DWORD len = GetModuleFileNameA(nullptr, appExe, sizeof(appExe));
	return string(appExe, len);
#endif
}

std::wstring BaseClientCore::GetDllDir()
{
#ifndef _WIN32
	// The folder of the shared object this function is in
	Dl_info info{};
	if (!dladdr((void*)&BaseClientCore::GetDllDir, &info) || !info.dli_fname)
		return {};
	string path = info.dli_fname;
	path = path.substr(0, path.find_last_of('/') + 1);
	return Utf8ToWide(path);
#else
	wchar_t buffer[MAX_PATH];
	DWORD len = GetModuleFileNameW(openovr_module_id, buffer, MAX_PATH);
	wstring dllpath(buffer, len);
	return dllpath.substr(0, dllpath.find_last_of('\\') + 1);
#endif
}
