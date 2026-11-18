#include "stdafx.h"

#include "BuiltInResource.h"

std::vector<char> ReadBuiltInResource(int id, int type)
{
#ifdef _WIN32
	// The module this code is in, found by an address in it
	HMODULE module = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&ReadBuiltInResource, &module))
		return {};
	HRSRC ref = FindResource(module, MAKEINTRESOURCE(id), MAKEINTRESOURCE(type));
	if (!ref)
		return {};
	const char* data = (const char*)LoadResource(module, ref);
	DWORD size = SizeofResource(module, ref);
	if (!data || !size)
		return {};
	return std::vector<char>(data, data + size);
#else
	(void)type;
	const char* start = nullptr;
	const char* end = nullptr;
	FindResourceLinux(id, &start, &end);
	if (!start || !end || end <= start)
		return {};
	return std::vector<char>(start, end);
#endif
}
