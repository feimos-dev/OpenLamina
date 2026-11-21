#include "OwnFrameLoop.h"

using namespace std::chrono;

namespace {

// About one frame at 90 Hz: the wait between tries while no frame can run (the session isn't running, the game's
// frame is open), and the pace when the runtime gave no display period
constexpr milliseconds kRetry{ 11 };

} // namespace

// The constructor without a runner, which runs XrBackend's frames, is in XrBackend.cpp

OwnFrameLoop::OwnFrameLoop(Mode mode, LayerSource source, milliseconds idleAfter, FrameRunner runner, ContentCheck hasContent)
    : mode(mode), source(std::move(source)), idleAfter(idleAfter), runner(std::move(runner)), hasContent(std::move(hasContent))
{
	// A Vulkan queue or an OpenGL context is the game's: used only from inside its calls, so no thread of their own
	if (mode != Mode::InCallsOnly)
		thread = std::thread([this] { Thread(); });
}

void OwnFrameLoop::RequestStop()
{
	{
		std::lock_guard<std::mutex> lock(stateLock);
		stopping = true;
	}
	wake.notify_all();
}

OwnFrameLoop::~OwnFrameLoop()
{
	RequestStop();
	if (thread.joinable())
		thread.join();
}

void OwnFrameLoop::OnAppFrameStart()
{
	std::lock_guard<std::mutex> lock(stateLock);
	appStarted = true;
	lastAppActivity = steady_clock::now();
}

void OwnFrameLoop::OnAppFrameEnd()
{
	std::lock_guard<std::mutex> lock(stateLock);
	lastAppActivity = steady_clock::now();
}

bool OwnFrameLoop::IdleNow(steady_clock::time_point now) const
{
	if (mode == Mode::EveryFrame)
		return true;
	if (mode == Mode::UntilAppFrame)
		return !appStarted;
	return appStarted && now - lastAppActivity >= idleAfter;
}

void OwnFrameLoop::Pace(steady_clock::time_point frameStart, XrDuration period)
{
	std::unique_lock<std::mutex> lock(stateLock);
	if (period <= 0) {
		nextFrameDue = {};
		wake.wait_until(lock, frameStart + kRetry, [this] { return stopping; });
		return;
	}
	// A runtime blocks in xrWaitFrame until the next frame is due; this keeps to the display rate where it doesn't. On
	// a schedule rather than a period after each frame, since a wait may last longer than asked (Windows' timer ticks
	// are about 16 ms): the next one is then shorter, or none. More than a few frames behind (a slow frame, a gap), the
	// schedule starts again.
	steady_clock::duration step = duration_cast<steady_clock::duration>(nanoseconds(period));
	if (nextFrameDue == steady_clock::time_point{} || nextFrameDue + 4 * step < frameStart)
		nextFrameDue = frameStart;
	nextFrameDue += step;
	wake.wait_until(lock, nextFrameDue, [this] { return stopping; });
}

void OwnFrameLoop::Thread()
{
	std::unique_lock<std::mutex> lock(stateLock);
	// Until the loop is stopped; each pass runs at most one frame, or waits
	while (!stopping) {
		steady_clock::time_point now = steady_clock::now();
		if (runningWhile > 0 || !IdleNow(now)) {
			// Until the game has been idle long enough, or (before its first frame, or while RunWhile runs the frames) a
			// while: OnAppFrameStart doesn't wake this, so a frame in the meantime just moves the time on
			steady_clock::time_point until = now + idleAfter;
			if (runningWhile == 0 && appStarted)
				until = lastAppActivity + idleAfter;
			if (until <= now)
				until = now + kRetry;
			wake.wait_until(lock, until);
			continue;
		}
		lock.unlock();

		// Checked without stateLock: it reads the overlays, whose holders may report a game frame meanwhile
		if (mode == Mode::WhenAppIdle && hasContent && !hasContent()) {
			lock.lock();
			wake.wait_until(lock, steady_clock::now() + kRetry, [this] { return stopping; });
			continue;
		}

		steady_clock::time_point start = steady_clock::now();
		XrDuration period = !runner ? 0 : runner(source, [this] {
			std::lock_guard<std::mutex> g(stateLock);
			return !stopping && runningWhile == 0 && IdleNow(steady_clock::now());
		});
		if (period > 0)
			framesRun++;
		Pace(start, period);

		lock.lock();
	}
}

void OwnFrameLoop::RunWhile(const std::function<bool()>& keepGoing)
{
	{
		std::lock_guard<std::mutex> lock(stateLock);
		runningWhile++;
	}
	// Bounded by keepGoing, and by the loop stopping
	while (true) {
		{
			std::lock_guard<std::mutex> lock(stateLock);
			if (stopping)
				break;
		}
		if (!keepGoing())
			break;
		steady_clock::time_point start = steady_clock::now();
		XrDuration period = !runner ? 0 : runner(source, [this] {
			std::lock_guard<std::mutex> g(stateLock);
			return !stopping;
		});
		if (period > 0)
			framesRun++;
		Pace(start, period);
	}
	{
		std::lock_guard<std::mutex> lock(stateLock);
		runningWhile--;
	}
	wake.notify_all();
}
