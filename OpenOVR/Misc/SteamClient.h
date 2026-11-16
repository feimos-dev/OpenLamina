#pragma once

#include <string>

// The Steam client's own programs (steam, steamwebhelper) connect to the VR runtime by themselves, at any time and
// with no game running, only to watch it: they never run a scene, so they never need the OpenXR runtime.

// Whether the program at this path (a file name will do) is one of them: steam or steamwebhelper, with or without
// .exe, in any case, in any folder
bool oc_is_steam_client_program(const std::string& program);

// Whether this program is one of them
bool oc_is_steam_client_program();
