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
	}
	memcpy(out, &stats, std::min<size_t>(size, sizeof(stats)));
}

uint32_t FrameStats::Timings(OOVR_Compositor_FrameTiming* out, uint32_t count) const
{
	if (!out || count == 0)
		return 0;
	size_t element = out->m_nSize;
	if (element < offsetof(OOVR_Compositor_FrameTiming, m_flSystemTimeInSeconds))
		return 0;
	std::lock_guard<std::mutex> g(lock);
	uint32_t n = (uint32_t)std::min<size_t>(count, history.size());
	auto* bytes = reinterpret_cast<uint8_t*>(out);
	for (uint32_t i = 0; i < n; i++) {
		uint8_t* dst = bytes + i * element;
		memset(dst, 0, element);
		memcpy(dst, &history[i], std::min(element, sizeof(OOVR_Compositor_FrameTiming)));
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
