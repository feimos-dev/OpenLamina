// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "OwnFrameLoop.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace {
using namespace std::chrono_literals;

// A frame runner that counts the frames it was asked for and wanted, and the thread they ran on
struct FakeFrames {
	std::atomic<int> frames{ 0 };
	std::atomic<std::thread::id> lastThread{};

	OwnFrameLoop::FrameRunner Runner()
	{
		return [this](const OwnFrameLoop::LayerSource& source, const std::function<bool()>& wanted) -> XrDuration {
			if (!wanted())
				return 0;
			std::vector<const XrCompositionLayerBaseHeader*> layers;
			if (source)
				source(1, layers);
			frames++;
			lastThread = std::this_thread::get_id();
			return 11'000'000;
		};
	}
};

bool WaitFor(const std::function<bool()>& pred, std::chrono::milliseconds timeout = 2s)
{
	auto end = std::chrono::steady_clock::now() + timeout;
	while (!pred()) {
		if (std::chrono::steady_clock::now() > end)
			return false;
		std::this_thread::sleep_for(1ms);
	}
	return true;
}
} // namespace

TEST_CASE("OwnFrameLoop: no frames before the game's first, then frames once it has been idle")
{
	FakeFrames fake;
	OwnFrameLoop loop(OwnFrameLoop::Mode::WhenAppIdle, nullptr, 50ms, fake.Runner());
	std::this_thread::sleep_for(150ms);
	CHECK(fake.frames == 0);

	loop.OnAppFrameStart();
	CHECK(WaitFor([&] { return fake.frames > 0; }));
	CHECK(fake.lastThread.load() != std::this_thread::get_id());

	// While the game keeps running frames, at most the one already in flight
	int before = fake.frames;
	for (int i = 0; i < 20; i++) {
		loop.OnAppFrameStart();
		std::this_thread::sleep_for(10ms);
		loop.OnAppFrameEnd();
	}
	CHECK(fake.frames <= before + 1);
}

TEST_CASE("OwnFrameLoop: Always runs frames without the game, and stops when destroyed")
{
	FakeFrames fake;
	std::atomic<int> sourced{ 0 };
	auto start = std::chrono::steady_clock::now();
	{
		OwnFrameLoop loop(
		    OwnFrameLoop::Mode::Always, [&](XrTime, std::vector<const XrCompositionLayerBaseHeader*>&) { sourced++; }, 100ms,
		    fake.Runner());
		CHECK(WaitFor([&] { return fake.frames >= 3; }));
	}
	CHECK(std::chrono::steady_clock::now() - start < 2s);
	int after = fake.frames;
	std::this_thread::sleep_for(50ms);
	CHECK(fake.frames == after);
	CHECK(sourced == after);
}

TEST_CASE("OwnFrameLoop: RunWhile runs the frames on the calling thread")
{
	FakeFrames fake;
	OwnFrameLoop loop(OwnFrameLoop::Mode::WhenAppIdle, nullptr, 100ms, fake.Runner());
	int calls = 0;
	loop.RunWhile([&] { return calls++ < 5; });
	CHECK(fake.frames == 5);
	CHECK(fake.lastThread.load() == std::this_thread::get_id());
	CHECK(loop.FramesRun() == 5);
}
