#include "stdafx.h"

#include "FrameStats.h"

#include <algorithm>
#include <cstring>

namespace {
// The time a runtime's compositor needs before the display, as GetFrameTiming reports it
constexpr double kCompositorMs = 1.5;
} // namespace

FrameStats& FrameStats::Get()
{
	static FrameStats stats;
	return stats;
}

void FrameStats::OnFrameStart()
{
	{
		std::lock_guard<std::mutex> g(lock);
		framesStarted++;
	}
	frameStarted.notify_all();
}

void FrameStats::OnAppFrame(const OOVR_Compositor_FrameTiming& timing, uint32_t droppedSlots, Clock::time_point waitReturned, double period)
{
	std::lock_guard<std::mutex> g(lock);
	dropped += droppedSlots;
	history.push_front(timing);
	history.front().m_nSize = sizeof(OOVR_Compositor_FrameTiming);
	if (history.size() > kHistory)
		history.pop_back();
	lastWaitReturned = waitReturned;
	periodMs = period;
}

void FrameStats::OnAppPresent()
{
	std::lock_guard<std::mutex> g(lock);
	presents++;
}

void FrameStats::OnAppFrameEnd(const FrameEnd& frame)
{
	std::lock_guard<std::mutex> g(lock);
	if (frame.submitted) {
		appSubmits++;
		// The sums the cumulative stats divide by the submitted frames: no GPU times without GPU timers
		sumCompositorCpuMs += frame.presentMs;
		sumTargetMs += frame.targetMs;
		sumAppCpuMs += frame.cpuMs;
		if (frame.withDepth)
			framesWithDepth++;
	}
	for (OOVR_Compositor_FrameTiming& t : history) {
		if (t.m_nFrameIndex != frame.frameIndex)
			continue;
		t.m_flSubmitFrameMs = frame.submitMs;
		t.m_flPresentCallCpuMs = frame.presentMs;
		t.m_flNewFrameReadyMs = frame.newFrameReadyMs;
		t.m_nNumFramePresents = frame.presented ? 1 : 0;
		return;
	}
}

void FrameStats::OnOwnFrame()
{
	std::lock_guard<std::mutex> g(lock);
	presents++;
}

void FrameStats::Cumulative(uint32_t pid, OOVR_Compositor_CumulativeStats* out, uint32_t size) const
{
	if (!out || size == 0)
		return;
	OOVR_Compositor_CumulativeStats stats{};
	stats.m_nPid = pid;
	{
		std::lock_guard<std::mutex> g(lock);
		stats.m_nNumFramePresents = presents;
		stats.m_nNumDroppedFrames = dropped;
		stats.m_nNumFrameSubmits = appSubmits;
		stats.m_flSumCompositorCPUTimeMS = sumCompositorCpuMs;
		stats.m_flSumTargetFrameTimes = sumTargetMs;
		stats.m_flSumApplicationCPUTimeMS = sumAppCpuMs;
		stats.m_nNumFramesWithDepth = framesWithDepth;
	}
	memset(out, 0, size);
	memcpy(out, &stats, std::min<size_t>(size, sizeof(stats)));
}

uint32_t FrameStats::Timings(OOVR_Compositor_FrameTiming* out, uint32_t count) const
{
	if (!out || count == 0)
		return 0;
	size_t element = out->m_nSize;
	if (element < offsetof(OOVR_Compositor_FrameTiming, m_flSystemTimeInSeconds))
		return 0;
	// As GetFrameTiming: an m_nSize bigger than the newest struct (padded to 8 bytes) is taken as ours, as the game's
	// array can't be of elements bigger than that
	constexpr size_t newestSize = (sizeof(vr::Compositor_FrameTiming) + 7) & ~(size_t)7;
	if (element > newestSize) {
		OOVR_LOG_ONCEF("GetFrameTimings: m_nSize %u is bigger than the newest OpenVR struct, using %u", out->m_nSize,
		    (uint32_t)sizeof(OOVR_Compositor_FrameTiming));
		element = sizeof(OOVR_Compositor_FrameTiming);
	}
	std::lock_guard<std::mutex> g(lock);
	uint32_t n = (uint32_t)std::min<size_t>(count, history.size());
	auto* bytes = reinterpret_cast<uint8_t*>(out);
	for (uint32_t i = 0; i < n; i++) {
		uint8_t* dst = bytes + i * element;
		memset(dst, 0, element);
		memcpy(dst, &history[n - 1 - i], std::min(element, sizeof(OOVR_Compositor_FrameTiming)));
		reinterpret_cast<OOVR_Compositor_FrameTiming*>(dst)->m_nSize = (uint32_t)element;
	}
	return n;
}

float FrameStats::SecondsRemaining(Clock::time_point now) const
{
	std::lock_guard<std::mutex> g(lock);
	if (lastWaitReturned == Clock::time_point{} || periodMs <= 0)
		return 0.0f;
	double dueMs = periodMs - kCompositorMs;
	double sinceMs = std::chrono::duration<double, std::milli>(now - lastWaitReturned).count();
	return (float)std::max(0.0, (dueMs - sinceMs) / 1000.0);
}

bool FrameStats::WaitForFrame(std::chrono::milliseconds timeout)
{
	std::unique_lock<std::mutex> g(lock);
	uint64_t seen = framesStarted;
	return frameStarted.wait_for(g, timeout, [&] { return framesStarted != seen; });
}

bool FrameStats::AnyFrame() const
{
	std::lock_guard<std::mutex> g(lock);
	return framesStarted != 0;
}
