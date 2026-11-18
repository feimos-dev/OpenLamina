#pragma once
// What the compositor calls report about frames: counts since start, the recent frames' timing, how long is left of
// the current frame, and a wait for the next frame to start. Shared by the game's frames and OpenLamina's own.

#include "Reimpl/BaseCompositor.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>

class FrameStats {
public:
	static constexpr size_t kHistory = 128;
	using Clock = std::chrono::steady_clock;

	static FrameStats& Get();

	// A frame starts: the game's (WaitGetPoses) or OpenLamina's own. Wakes WaitForFrame.
	void OnFrameStart();
	// The game's frame after its xrWaitFrame returned at waitReturned: its timing (as GetFrameTiming fills it in, with
	// m_nSize the full struct), the display slots the runtime skipped before it, and the display period
	void OnAppFrame(const OOVR_Compositor_FrameTiming& timing, uint32_t droppedSlots, Clock::time_point waitReturned, double periodMs);
	// The game's frame was shown: xrEndFrame took it with the game's eye textures
	void OnAppPresent();
	// One of OpenLamina's own frames was shown
	void OnOwnFrame();

	// The counts, zeroed within size bytes (older SDKs have a shorter struct), with pid
	void Cumulative(uint32_t pid, OOVR_Compositor_CumulativeStats* out, uint32_t size) const;
	// Up to count of the game's recent frames, newest first, into an array of elements of out->m_nSize bytes each (an
	// element size under the struct's first fields, or count 0, gives 0); the number written
	uint32_t Timings(OOVR_Compositor_FrameTiming* out, uint32_t count) const;
	// Seconds left until the current frame is due: one display period after the last xrWaitFrame returned, less the
	// time the runtime's compositor needs; 0 once it's due or before any frame
	float SecondsRemaining(Clock::time_point now) const;
	// Waits for the next frame to start, at most timeout; false if none did
	bool WaitForFrame(std::chrono::milliseconds timeout);
	// Whether any frame has started yet
	bool AnyFrame() const;

private:
	mutable std::mutex lock;
	std::condition_variable frameStarted;
	uint64_t framesStarted = 0;
	uint32_t presents = 0, dropped = 0;
	std::deque<OOVR_Compositor_FrameTiming> history; // newest first
	Clock::time_point lastWaitReturned{};
	double periodMs = 0;
};
