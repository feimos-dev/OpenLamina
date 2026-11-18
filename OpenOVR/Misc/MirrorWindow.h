#pragma once
// A desktop window showing the game's left eye, as SteamVR's mirror window (IVRCompositor::ShowMirrorWindow). Drawn
// on a thread of its own from images handed to it; closing the window hides it. Windows only: elsewhere it's never
// visible.

#include "Image.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

class MirrorWindow {
public:
	static MirrorWindow& Get();

	void Show();
	void Hide(); // joins the window's thread
	bool Visible() const { return visible; }

	// Whether a new image is wanted now: visible, and at most 30 a second
	bool WantsFrame();
	// The image to show next
	void Offer(Image image);

	~MirrorWindow();

	// For the window's procedure: the image to draw, and the window closed by the user
	void CopyImage(Image& out);
	void MarkClosed();

private:
	void Thread();

	std::atomic<bool> visible{ false };
	std::atomic<bool> closing{ false };
	std::mutex lock; // the image, and starting and stopping the thread
	Image image;
	std::chrono::steady_clock::time_point lastFrame{};
	std::thread thread;
	void* window = nullptr; // HWND, while the thread runs
};
