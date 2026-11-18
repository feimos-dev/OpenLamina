#pragma once
// Frames run by OpenLamina itself when the game doesn't run any (loading, a stall, a blocking call), with layers of
// OpenLamina's choosing. WhenAppIdle starts them idleAfter the game's last frame call, so a game that keeps calling
// WaitGetPoses without submitting, or stalls with its frame open, gets none.

#include <openxr/openxr.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class OwnFrameLoop {
public:
	// Adds this frame's layers for displayTime; they must stay valid until the frame has ended
	using LayerSource = std::function<void(XrTime displayTime, std::vector<const XrCompositionLayerBaseHeader*>& layers)>;

	// Runs one frame with the source's layers, unless wanted() turns false before it starts (checked with the frame
	// owned, so a game's frame can't start in between). Answers the frame's predicted display period, or 0 if no frame
	// ran. Without one, the current XrBackend's (RunOwnFrame).
	using FrameRunner = std::function<XrDuration(const LayerSource& source, const std::function<bool()>& wanted)>;

	// WhenAppIdle runs frames only while this holds: with nothing of its own to show, the runtime keeps the game's last
	// frame rather than getting empty ones
	using ContentCheck = std::function<bool()>;

	enum class Mode {
		WhenAppIdle, // frames once the game has run one and then not called WaitGetPoses for idleAfter
		Always, // every frame (overlay applications, which have no frames of their own)
	};

	OwnFrameLoop(Mode mode, LayerSource source, std::chrono::milliseconds idleAfter = std::chrono::milliseconds(100));
	OwnFrameLoop(Mode mode, LayerSource source, std::chrono::milliseconds idleAfter, FrameRunner runner, ContentCheck hasContent = nullptr);
	OwnFrameLoop(const OwnFrameLoop&) = delete;
	OwnFrameLoop& operator=(const OwnFrameLoop&) = delete;

	// From WaitGetPoses, with the frame owned: the game drives from here (no own frame is in flight, since the frame
	// is owned), and idleAfter counts from now
	void OnAppFrameStart();
	// After the game's xrEndFrame: idleAfter counts from now too (a slow frame isn't a pause)
	void OnAppFrameEnd();

	// Runs frames on the calling thread while keepGoing() holds (a blocking call such as ShowMessageOverlay), whatever
	// the mode, and until the loop is stopped
	void RunWhile(const std::function<bool()>& keepGoing);

	// Own frames run so far (for logs and tests)
	uint64_t FramesRun() const { return framesRun; }

	// Stops and joins its thread
	~OwnFrameLoop();

private:
	void Thread();
	// Whether the loop's thread should run a frame now; with stateLock held
	bool IdleNow(std::chrono::steady_clock::time_point now) const;
	// Waits until the next frame is due after one that took period (or about a frame if none ran), or the loop stops
	void Pace(std::chrono::steady_clock::time_point frameStart, XrDuration period);

	const Mode mode;
	const LayerSource source;
	const std::chrono::milliseconds idleAfter;
	const FrameRunner runner;
	const ContentCheck hasContent;

	mutable std::mutex stateLock;
	std::condition_variable wake;
	bool stopping = false;
	bool appStarted = false; // the game has run a frame
	int runningWhile = 0; // RunWhile calls in progress: the thread leaves the frames to them
	std::chrono::steady_clock::time_point lastAppActivity{};
	std::chrono::steady_clock::time_point nextFrameDue{}; // Pace's schedule
	std::atomic<uint64_t> framesRun{ 0 };

	std::thread thread;
};
