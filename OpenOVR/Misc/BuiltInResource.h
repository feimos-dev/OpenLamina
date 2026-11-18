#pragma once
// The data built into OpenLamina (resources.h): the keyboard's layouts and font, the hand models

#include <vector>

// The resource's bytes, from the module this code is in (OpenLamina's DLL, or a test program); empty if it has none.
// On Linux the type isn't needed (each id is one file, resources_linux.S).
std::vector<char> ReadBuiltInResource(int id, int type);
