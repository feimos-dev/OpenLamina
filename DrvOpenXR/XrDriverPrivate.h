//
// Created by ZNix on 25/10/2020.
//

#pragma once

// FIXME Make it so that this header can be used in the Vulkan compositor - unfortunate and we should probably fix that at some point
#include "pub/DrvOpenXR.h"

#include "../OpenOVR/Misc/xrutil.h"
#include "../OpenOVR/logging.h"

#ifndef _WIN32
#include "../OpenOVR/linux_funcs.h"
#endif

namespace DrvOpenXR {
// XR_SUCCESS, or what xrCreateSession answered (no session then: the caller decides what next)
XrResult SetupSession();
void ShutdownSession();
// xrGetSystem for the headset again (after a loss): XR_SUCCESS once it is there
XrResult FindSystem();
// After the instance was lost (its session already ended): the old instance destroyed, a new one made on the same
// extensions, with its function pointers, system and views. XR_SUCCESS once there's a headset on it.
XrResult RemakeInstance();
void FullShutdown();
} // namespace DrvOpenXR
