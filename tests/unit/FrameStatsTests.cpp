// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "Misc/FrameStats.h"

#include <algorithm>
#include <thread>
#include <vector>

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
	// Twelve of the game's frames start, eleven are submitted and ten are shown
	for (uint32_t i = 0; i < 12; i++) {
		s.OnAppFrame(Timing(i), i == 3 || i == 7 ? 1 : 0, now, 11.1);
		if (i < 10)
			s.OnAppPresent();
		FrameStats::FrameEnd end;
		end.frameIndex = i;
		end.submitted = i < 11;
		end.presented = i < 10;
		end.withDepth = i < 4;
		end.cpuMs = 2.0f;
		end.targetMs = 11.0f;
		end.presentMs = 0.5f;
		s.OnAppFrameEnd(end);
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
	const auto* full = (OOVR_Compositor_CumulativeStats*)big.data();
	CHECK(full->m_nNumFrameSubmits == 11);
	CHECK(full->m_nNumFramesWithDepth == 4);
	CHECK(full->m_flSumApplicationCPUTimeMS == doctest::Approx(22.0));
	CHECK(full->m_flSumTargetFrameTimes == doctest::Approx(121.0));
	CHECK(full->m_flSumCompositorCPUTimeMS == doctest::Approx(5.5));
	CHECK(full->m_flSumApplicationGPUTimeMS == 0.0);
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

TEST_CASE("FrameStats: a frame's submit and present times are filled in when it ends")
{
	FrameStats s;
	auto now = FrameStats::Clock::now();
	OOVR_Compositor_FrameTiming t = Timing(7);
	t.m_nNumFramePresents = 1;
	s.OnAppFrame(t, 0, now, 11.1);
	s.OnAppFrame(Timing(8), 0, now, 11.1);
	// Frame 7 ends late, after 8 has started; its own record is the one filled in
	FrameStats::FrameEnd end;
	end.frameIndex = 7;
	end.submitMs = 0.5f;
	end.presentMs = 1.25f;
	end.newFrameReadyMs = 9.0f;
	end.submitted = true;
	s.OnAppFrameEnd(end);
	std::vector<OOVR_Compositor_FrameTiming> out(2);
	out[0].m_nSize = sizeof(OOVR_Compositor_FrameTiming);
	REQUIRE(s.Timings(out.data(), 2) == 2);
	CHECK(out[0].m_nFrameIndex == 7);
	CHECK(out[0].m_flSubmitFrameMs == 0.5f);
	CHECK(out[0].m_flPresentCallCpuMs == 1.25f);
	CHECK(out[0].m_flNewFrameReadyMs == 9.0f);
	CHECK(out[0].m_nNumFramePresents == 0);
	CHECK(out[1].m_flSubmitFrameMs == 0.0f);
	// A frame no longer kept changes nothing
	end.frameIndex = 3;
	end.submitMs = 1.0f;
	s.OnAppFrameEnd(end);
	REQUIRE(s.Timings(out.data(), 2) == 2);
	CHECK(out[1].m_nFrameIndex == 8);
	CHECK(out[1].m_flSubmitFrameMs == 0.0f);
}

TEST_CASE("FrameStats: an m_nSize bigger than the newest struct is taken as ours")
{
	FrameStats s;
	const auto now = FrameStats::Clock::now();
	s.OnAppFrame(Timing(1), 0, now, 11.1);
	s.OnAppFrame(Timing(2), 0, now, 11.1);
	// Two entries and guard bytes after them
	constexpr size_t used = 2 * sizeof(OOVR_Compositor_FrameTiming);
	std::vector<uint8_t> buffer(used + 4096, 0xEE);
	auto* out = reinterpret_cast<OOVR_Compositor_FrameTiming*>(buffer.data());
	out->m_nSize = 4096;
	REQUIRE(s.Timings(out, 2) == 2);
	CHECK(out[0].m_nSize == sizeof(OOVR_Compositor_FrameTiming));
	CHECK(out[0].m_nFrameIndex + out[1].m_nFrameIndex == 3);
	CHECK(std::all_of(buffer.begin() + used, buffer.end(), [](uint8_t b) { return b == 0xEE; }));
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
