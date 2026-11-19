#include "stdafx.h"
#define BASE_IMPL
#include "BaseExtendedDisplay.h"
#include "BaseSystem.h"
#include "Drivers/Backend.h"

#include <string>

// One eye's size: the render target size the application is given
static void EyeSize(uint32_t* width, uint32_t* height)
{
	*width = *height = 0;
	BackendManager::Instance().GetPrimaryHMD()->GetRecommendedRenderTargetSize(width, height);
}

// Unreal Engine uses this for some silly reason
void BaseExtendedDisplay::GetWindowBounds(int32_t* pnX, int32_t* pnY, uint32_t* pnWidth, uint32_t* pnHeight)
{
	// Make up something that sounds about right: the two eyes side by side
	uint32_t width, height;
	EyeSize(&width, &height);
	if (pnX)
		*pnX = 0;
	if (pnY)
		*pnY = 0;
	if (pnWidth)
		*pnWidth = width * 2;
	if (pnHeight)
		*pnHeight = height;
}

void BaseExtendedDisplay::GetEyeOutputViewport(vr::EVREye eEye, uint32_t* pnX, uint32_t* pnY, uint32_t* pnWidth, uint32_t* pnHeight)
{
	// Each eye's half of the made-up window GetWindowBounds gives, left eye first
	uint32_t width, height;
	EyeSize(&width, &height);
	if (pnX)
		*pnX = eEye == vr::Eye_Right ? width : 0;
	if (pnY)
		*pnY = 0;
	if (pnWidth)
		*pnWidth = width;
	if (pnHeight)
		*pnHeight = height;
}

void BaseExtendedDisplay::GetDXGIOutputInfo(int32_t* pnAdapterIndex, int32_t* pnAdapterOutputIndex)
{
	// The headset isn't a desktop output: the OpenXR runtime's adapter (else the first), and its first output, as SteamVR
	// answers in direct mode
	if (pnAdapterIndex) {
		uint64_t luid = 0;
		int32_t index = BaseSystem::_RuntimeAdapterLuid(&luid) ? BaseSystem::_DxgiAdapterIndexOf(luid) : -1;
		*pnAdapterIndex = index >= 0 ? index : 0;
	}
	if (pnAdapterOutputIndex)
		*pnAdapterOutputIndex = 0;
}
