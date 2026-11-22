#pragma once

// Every tracked device property of the newest OpenVR header, with the type its name gives it (gen_properties.py)

#include "IVRSystem_022.h"

#include <cstdint>
#include <vector>

struct PropertyRow {
	int32_t value;
	const char* name; // without "Prop_"
	vr::PropertyTypeTag_t tag; // the getter: string, bool, float, int32, uint64, matrix; an array of the tag's type
	bool array; // read through GetArrayTrackedDeviceProperty
};

const std::vector<PropertyRow>& AllProperties();
