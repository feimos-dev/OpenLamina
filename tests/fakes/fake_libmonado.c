/* A stand-in for Monado's libmonado (LibMonadoTests.cpp), scripted through environment variables:
 *   FAKE_MND_MAJOR       the major version it reports (default 1)
 *   FAKE_MND_BATTERY     "present,charging,charge" for every device (default "0,0,0"), or up to three of them
 *                        separated by ';', one per device
 *   FAKE_MND_NO_BATTERY  set: the battery call fails with MND_ERROR_OPERATION_FAILED (-4), as Monado's does for a
 *                        device without battery status
 *   FAKE_MND_SAME_NAMES  set: every device is called "Fake Controller"
 *   FAKE_MND_LOG         a file each mnd_root_set_reference_space_offset call is added to, as a line
 *                        "set_reference_space_offset <type> <px> <py> <pz> <qx> <qy> <qz> <qw>"
 *   FAKE_MND_NO_OFFSETS  set: the reference space offset calls fail with MND_ERROR_INVALID_OPERATION (-2)
 *   FAKE_MND_NO_GET      set: only the get call of the reference space offsets fails (as without libmonado 1.3)
 *   FAKE_MND_NO_SET      set: only the set call of the reference space offsets fails
 * Three devices: 0 "Fake HMD" (head), 1 "Fake Left" (left), 2 "Fake Right" (right); serials SN0, SN1, SN2.
 * fake_mnd_battery_calls() answers how often the battery was asked for. Each reference space's offset is kept
 * (identity at first) and given back, while the library stays loaded; fake_mnd_restart() puts them back to identity,
 * as a restart of Monado's service does. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define FAKE_API __declspec(dllexport)
#else
#define FAKE_API __attribute__((visibility("default")))
#endif

/* The process's environment: under /MT this library has a C runtime of its own, whose getenv doesn't see the tests'
 * _putenv_s, so on Windows it's read from the process itself */
static const char* fake_getenv(const char* name)
{
#ifdef _WIN32
	static char buffers[2][256];
	static int next;
	char* buffer = buffers[next++ % 2];
	DWORD n = GetEnvironmentVariableA(name, buffer, sizeof(buffers[0]));
	return n > 0 && n < sizeof(buffers[0]) ? buffer : NULL;
#else
	return getenv(name);
#endif
}

static const char* names[] = { "Fake HMD", "Fake Left", "Fake Right" };
static const char* serials[] = { "SN0", "SN1", "SN2" };
static const char* roles[] = { "head", "left", "right" };
static int root_storage;

/* void, as libmonado's own */
FAKE_API void mnd_api_get_version(uint32_t* major, uint32_t* minor, uint32_t* patch)
{
	const char* m = fake_getenv("FAKE_MND_MAJOR");
	*major = m ? (uint32_t)atoi(m) : 1;
	*minor = 2;
	*patch = 0;
}

FAKE_API int mnd_root_create(void** out_root)
{
	*out_root = &root_storage;
	return 0;
}

FAKE_API void mnd_root_destroy(void** root)
{
	*root = NULL;
}

FAKE_API int mnd_root_get_device_count(void* root, uint32_t* out_count)
{
	(void)root;
	*out_count = 3;
	return 0;
}

FAKE_API int mnd_root_get_device_info_string(void* root, uint32_t index, int prop, const char** out)
{
	(void)root;
	if (index > 2)
		return -1;
	if (prop == 0)
		*out = fake_getenv("FAKE_MND_SAME_NAMES") ? "Fake Controller" : names[index];
	else if (prop == 1)
		*out = serials[index];
	else
		return -1;
	return 0;
}

FAKE_API int mnd_root_get_device_from_role(void* root, const char* role, int32_t* out_index)
{
	(void)root;
	for (int i = 0; i < 3; i++) {
		if (strcmp(role, roles[i]) == 0) {
			*out_index = i;
			return 0;
		}
	}
	*out_index = -1;
	return -1;
}

static int batteryCalls;

FAKE_API int fake_mnd_battery_calls(void)
{
	return batteryCalls;
}

FAKE_API int mnd_root_get_device_battery_status(void* root, uint32_t index, bool* present, bool* charging, float* charge)
{
	(void)root;
	batteryCalls++;
	if (index > 2)
		return -1;
	if (fake_getenv("FAKE_MND_NO_BATTERY"))
		return -4; /* MND_ERROR_OPERATION_FAILED */
	int p = 0, c = 0;
	float level = 0;
	const char* b = fake_getenv("FAKE_MND_BATTERY");
	/* The index-th of the ';'-separated entries, or the only one */
	for (uint32_t i = 0; b && i < index && strchr(b, ';'); i++)
		b = strchr(b, ';') + 1;
	if (b)
		sscanf(b, "%d,%d,%f", &p, &c, &level);
	*present = p != 0;
	*charging = c != 0;
	*charge = level;
	return 0;
}

typedef struct {
	float orientation[4]; /* x, y, z, w */
	float position[3];
} fake_pose;

static fake_pose offsets[5] = {
	{ { 0, 0, 0, 1 }, { 0, 0, 0 } },
	{ { 0, 0, 0, 1 }, { 0, 0, 0 } },
	{ { 0, 0, 0, 1 }, { 0, 0, 0 } },
	{ { 0, 0, 0, 1 }, { 0, 0, 0 } },
	{ { 0, 0, 0, 1 }, { 0, 0, 0 } },
};

FAKE_API int mnd_root_set_reference_space_offset(void* root, int type, const fake_pose* offset)
{
	(void)root;
	if (type < 0 || type > 4 || fake_getenv("FAKE_MND_NO_OFFSETS") || fake_getenv("FAKE_MND_NO_SET"))
		return -2; /* MND_ERROR_INVALID_OPERATION */
	offsets[type] = *offset;
	const char* log = fake_getenv("FAKE_MND_LOG");
	FILE* f = log ? fopen(log, "a") : NULL;
	if (f) {
		fprintf(f, "set_reference_space_offset %d %g %g %g %g %g %g %g\n", type, offset->position[0], offset->position[1],
		    offset->position[2], offset->orientation[0], offset->orientation[1], offset->orientation[2], offset->orientation[3]);
		fclose(f);
	}
	return 0;
}

FAKE_API int mnd_root_get_reference_space_offset(void* root, int type, fake_pose* out_offset)
{
	(void)root;
	if (type < 0 || type > 4 || fake_getenv("FAKE_MND_NO_OFFSETS") || fake_getenv("FAKE_MND_NO_GET"))
		return -2;
	*out_offset = offsets[type];
	return 0;
}

FAKE_API int fake_mnd_restart(void)
{
	for (int i = 0; i < 5; i++) {
		fake_pose identity = { { 0, 0, 0, 1 }, { 0, 0, 0 } };
		offsets[i] = identity;
	}
	return 0;
}
