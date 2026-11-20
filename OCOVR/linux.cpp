//
// Created by ZNix on 14/02/2021.
//

#include "stdafx.h"

#include "plat.h"

alternativeCoreFactory_t PlatformGetAlternativeCoreFactory()
{
	// The per-app runtime choice is Windows-only (BaseClientCore::CheckAppEnabled), so this isn't reached
	OOVR_LOG("No per-app runtime list on Linux; using OpenLamina. To start one game on SteamVR, set its launch options to "
	         "VR_OVERRIDE=<dir> %command%, where <dir> is SteamVR's folder");
	return nullptr;
}
