#include "SteamClient.h"

#include <cctype>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

bool oc_is_steam_client_program(const std::string& program)
{
	// The file name without its folder or its .exe, in lower case
	std::string name = program.substr(program.find_last_of("/\\") + 1);
	for (char& c : name)
		c = (char)std::tolower((unsigned char)c);
	if (name.size() > 4 && name.compare(name.size() - 4, 4, ".exe") == 0)
		name.resize(name.size() - 4);
	return name == "steam" || name == "steamwebhelper";
}

bool oc_is_steam_client_program()
{
	static const bool steam = [] {
#ifdef _WIN32
		char path[MAX_PATH];
		DWORD len = GetModuleFileNameA(nullptr, path, sizeof(path));
		return oc_is_steam_client_program(std::string(path, len));
#else
		char path[4096];
		ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
		return oc_is_steam_client_program(len > 0 ? std::string(path, (size_t)len) : std::string());
#endif
	}();
	return steam;
}
