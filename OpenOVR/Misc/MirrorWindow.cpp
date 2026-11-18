#include "stdafx.h"

#include "MirrorWindow.h"

#ifdef _WIN32
#include <Windows.h>
#endif

#include <vector>

MirrorWindow& MirrorWindow::Get()
{
	static MirrorWindow mirror;
	return mirror;
}

MirrorWindow::~MirrorWindow()
{
	// Hidden at shutdown (XrBackend); joining here, at the DLL's unloading, would wait on the loader lock
	if (thread.joinable())
		thread.detach();
}

bool MirrorWindow::WantsFrame()
{
	if (!visible)
		return false;
	auto now = std::chrono::steady_clock::now();
	std::lock_guard<std::mutex> g(lock);
	if (now - lastFrame < std::chrono::milliseconds(33))
		return false;
	lastFrame = now;
	return true;
}

void MirrorWindow::Offer(Image next)
{
	std::lock_guard<std::mutex> g(lock);
	image = std::move(next);
}

#ifndef _WIN32

void MirrorWindow::Show() {}
void MirrorWindow::Hide() {}
void MirrorWindow::Thread() {}

#else

namespace {
const wchar_t kClass[] = L"OpenLaminaMirror";

LRESULT CALLBACK MirrorProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
} // namespace

void MirrorWindow::Show()
{
	std::lock_guard<std::mutex> g(lock);
	if (visible)
		return;
	// A window the user closed leaves its finished thread behind
	if (thread.joinable())
		thread.join();
	closing = false;
	visible = true;
	thread = std::thread([this] { Thread(); });
}

void MirrorWindow::Hide()
{
	std::thread finished;
	{
		std::lock_guard<std::mutex> g(lock);
		closing = true;
		finished = std::move(thread);
	}
	if (finished.joinable())
		finished.join();
	visible = false;
}

void MirrorWindow::Thread()
{
	HINSTANCE module = GetModuleHandleW(nullptr);
	WNDCLASSW wc{};
	wc.lpfnWndProc = MirrorProc;
	wc.hInstance = module;
	wc.lpszClassName = kClass;
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	RegisterClassW(&wc); // fails harmlessly when already registered
	HWND wnd = CreateWindowExW(0, kClass, L"OpenLamina mirror", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 960, 540,
	    nullptr, nullptr, module, nullptr);
	if (!wnd) {
		visible = false;
		return;
	}
	window = wnd;
	ShowWindow(wnd, SW_SHOWNOACTIVATE);

	// Until hidden or closed: redrawn about 30 times a second
	while (!closing && visible) {
		MsgWaitForMultipleObjects(0, nullptr, FALSE, 33, QS_ALLINPUT);
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		if (visible)
			InvalidateRect(wnd, nullptr, FALSE);
	}
	if (IsWindow(wnd))
		DestroyWindow(wnd);
	window = nullptr;
	visible = false;
}

namespace {
LRESULT CALLBACK MirrorProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(wnd, &ps);
		RECT client;
		GetClientRect(wnd, &client);
		Image shown;
		MirrorWindow::Get().CopyImage(shown);
		if (shown.Empty()) {
			FillRect(dc, &client, (HBRUSH)GetStockObject(BLACK_BRUSH));
		} else {
			// A DIB is BGRA, top-down with a negative height
			std::vector<uint8_t> bgra(shown.rgba.size());
			for (size_t i = 0; i < bgra.size(); i += 4) {
				bgra[i] = shown.rgba[i + 2];
				bgra[i + 1] = shown.rgba[i + 1];
				bgra[i + 2] = shown.rgba[i];
				bgra[i + 3] = 255;
			}
			BITMAPINFO info{};
			info.bmiHeader.biSize = sizeof(info.bmiHeader);
			info.bmiHeader.biWidth = (LONG)shown.width;
			info.bmiHeader.biHeight = -(LONG)shown.height;
			info.bmiHeader.biPlanes = 1;
			info.bmiHeader.biBitCount = 32;
			info.bmiHeader.biCompression = BI_RGB;
			SetStretchBltMode(dc, HALFTONE);
			StretchDIBits(dc, 0, 0, client.right, client.bottom, 0, 0, (int)shown.width, (int)shown.height, bgra.data(), &info,
			    DIB_RGB_COLORS, SRCCOPY);
		}
		EndPaint(wnd, &ps);
		return 0;
	}
	case WM_CLOSE:
		// Closing it hides it, as SteamVR's
		MirrorWindow::Get().MarkClosed();
		DestroyWindow(wnd);
		return 0;
	}
	return DefWindowProcW(wnd, msg, wp, lp);
}
} // namespace

#endif

void MirrorWindow::CopyImage(Image& out)
{
	std::lock_guard<std::mutex> g(lock);
	out = image;
}

void MirrorWindow::MarkClosed()
{
	visible = false;
}
