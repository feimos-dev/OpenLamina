#include "BaseHeadsetView.h"
#define BASE_IMPL
#include "../BaseCommon.h"
#include "Drivers/Backend.h"

#include <algorithm>
#include <mutex>

// The largest size the header allows
static const uint32_t kMaxWidth = 3840, kMaxHeight = 2160;

// What the application set. No view is rendered, but it reads back what it wrote, for as long as the process lives.
static struct {
	std::mutex lock;
	uint32_t width = 0, height = 0;
	OOVR_HeadsetViewMode_t mode = HeadsetViewMode_Both;
	bool cropped = false;
	float blendStart = 0.0f, blendEnd = 0.0f;
} state;

void BaseHeadsetView::SetHeadsetViewSize(uint32_t nWidth, uint32_t nHeight)
{
	std::lock_guard<std::mutex> guard(state.lock);
	state.width = std::min(nWidth, kMaxWidth);
	state.height = std::min(nHeight, kMaxHeight);
}

void BaseHeadsetView::GetHeadsetViewSize(uint32_t* pnWidth, uint32_t* pnHeight)
{
	std::lock_guard<std::mutex> guard(state.lock);
	if (pnWidth)
		*pnWidth = state.width;
	if (pnHeight)
		*pnHeight = state.height;
}

void BaseHeadsetView::SetHeadsetViewMode(OOVR_HeadsetViewMode_t eHeadsetViewMode)
{
	if (eHeadsetViewMode < HeadsetViewMode_Left || eHeadsetViewMode > HeadsetViewMode_Both)
		return;
	std::lock_guard<std::mutex> guard(state.lock);
	state.mode = eHeadsetViewMode;
}

OOVR_HeadsetViewMode_t BaseHeadsetView::GetHeadsetViewMode()
{
	std::lock_guard<std::mutex> guard(state.lock);
	return state.mode;
}

void BaseHeadsetView::SetHeadsetViewCropped(bool bCropped)
{
	std::lock_guard<std::mutex> guard(state.lock);
	state.cropped = bCropped;
}

bool BaseHeadsetView::GetHeadsetViewCropped()
{
	std::lock_guard<std::mutex> guard(state.lock);
	return state.cropped;
}

float BaseHeadsetView::GetHeadsetViewAspectRatio()
{
	// One eye's image, or both side by side
	uint32_t eyeWidth = 0, eyeHeight = 0;
	BackendManager::Instance().GetPrimaryHMD()->GetRecommendedRenderTargetSize(&eyeWidth, &eyeHeight);
	if (eyeWidth == 0 || eyeHeight == 0)
		return 1.0f;

	float aspect = (float)eyeWidth / (float)eyeHeight;
	std::lock_guard<std::mutex> guard(state.lock);
	return state.mode == HeadsetViewMode_Both ? aspect * 2 : aspect;
}

void BaseHeadsetView::SetHeadsetViewBlendRange(float flStartPct, float flEndPct)
{
	std::lock_guard<std::mutex> guard(state.lock);
	state.blendStart = std::clamp(flStartPct, 0.0f, 1.0f);
	state.blendEnd = std::clamp(flEndPct, 0.0f, 1.0f);
}

void BaseHeadsetView::GetHeadsetViewBlendRange(float* pStartPct, float* pEndPct)
{
	std::lock_guard<std::mutex> guard(state.lock);
	if (pStartPct)
		*pStartPct = state.blendStart;
	if (pEndPct)
		*pEndPct = state.blendEnd;
}
