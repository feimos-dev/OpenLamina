// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "Misc/FrameStats.h"

#include <thread>

using namespace std::chrono_literals;

namespace {
OOVR_Compositor_FrameTiming Timing(uint32_t index)
{
	OOVR_Compositor_FrameTiming t{};
	t.m_nSize = sizeof(t);
	t.m_nFrameIndex = index;
	return t;
}
} // namespace

TEST_CASE("FrameStats: counts the game's frames shown, its own frames and the dropped slots")
{
	FrameStats s;
	auto now = FrameStats::Clock::now();
	// Twelve of the game's frames start, and ten are shown
	for (uint32_t i = 0; i < 12; i++) {
		s.OnAppFrame(Timing(i), i == 3 || i == 7 ? 1 : 0, now, 11.1);
		if (i < 10)
			s.OnAppPresent();
	}
	s.OnOwnFrame();
	OOVR_Compositor_CumulativeStats c{};
	s.Cumulative(42, &c, sizeof(c));
	CHECK(c.m_nPid == 42);
	CHECK(c.m_nNumFramePresents == 11);
	CHECK(c.m_nNumDroppedFrames == 2);

	// A shorter struct (an older SDK) is written no further than its size
	uint32_t shortStats[3] = { 0xffffffff, 0xffffffff, 0xffffffff };
	s.Cumulative(42, (OOVR_Compositor_CumulativeStats*)shortStats, 8);
	CHECK(shortStats[0] == 42);
	CHECK(shortStats[1] == 11);
	CHECK(shortStats[2] == 0xffffffff);

	// A struct larger than ours is zeroed throughout
	std::vector<uint32_t> big(sizeof(c) / 4 + 8, 0xffffffff);
	s.Cumulative(42, (OOVR_Compositor_CumulativeStats*)big.data(), (uint32_t)(big.size() * 4));
	CHECK(big[0] == 42);
	for (size_t i = sizeof(c) / 4; i < big.size(); i++)
		CHECK(big[i] == 0);
	CHECK(((OOVR_Compositor_CumulativeStats*)big.data())->m_nNumFrameSubmits == 10);
}

TEST_CASE("FrameStats: timings oldest first, at most the history kept")
{
	FrameStats s;
	auto now = FrameStats::Clock::now();
	for (uint32_t i = 0; i < 200; i++)
		s.OnAppFrame(Timing(i), 0, now, 11.1);
	std::vector<OOVR_Compositor_FrameTiming> out(300);
	out[0].m_nSize = sizeof(OOVR_Compositor_FrameTiming);
	CHECK(s.Timings(out.data(), 300) == FrameStats::kHistory);
	CHECK(out[0].m_nFrameIndex == 200 - FrameStats::kHistory);
	CHECK(out[1].m_nFrameIndex == 201 - FrameStats::kHistory);
	CHECK(out[FrameStats::kHistory - 1].m_nFrameIndex == 199);
	CHECK(out[1].m_nSize == sizeof(OOVR_Compositor_FrameTiming));
	CHECK(s.Timings(out.data(), 0) == 0);

	// Fewer asked for than kept: the newest few, still oldest first
	CHECK(s.Timings(out.data(), 3) == 3);
	CHECK(out[0].m_nFrameIndex == 197);
	CHECK(out[2].m_nFrameIndex == 199);
}

TEST_CASE("FrameStats: seconds remaining are never negative")
{
	FrameStats s;
	auto now = FrameStats::Clock::now();
	CHECK(s.SecondsRemaining(now) == 0.0f);
	s.OnAppFrame(Timing(1), 0, now, 11.1);
	float left = s.SecondsRemaining(now + 2ms);
	CHECK(left > 0.007f);
	CHECK(left < 0.008f);
	CHECK(s.SecondsRemaining(now + 50ms) == 0.0f);
}

TEST_CASE("FrameStats: WaitForFrame returns when a frame starts, or after its timeout")
{
	FrameStats s;
	CHECK_FALSE(s.AnyFrame());
	CHECK_FALSE(s.WaitForFrame(20ms));
	std::thread t([&] {
		std::this_thread::sleep_for(20ms);
		s.OnFrameStart();
	});
	CHECK(s.WaitForFrame(2000ms));
	t.join();
	CHECK(s.AnyFrame());
}
