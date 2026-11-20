// OpenGL for the frame cases: a hidden window's context (WGL on Windows, GLX on Linux: DISPLAY's X server), current
// on the case's thread (OpenLamina takes the game's context from the thread that submits). Textures are filled with
// glTexImage2D, rows bottom-up as OpenGL has them. Windows' own OpenGL 1.1 isn't enough: a driver's OpenGL 3 or later
// is needed.

#include "GfxApi.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <GL/gl.h>
#else
#include <GL/glx.h>
#include <X11/Xlib.h>
#endif

#include <cstdio>

namespace {

constexpr GLenum kTexture2DArray = 0x8C1A, kTextureBinding2DArray = 0x8C1D, kDepthComponent32F = 0x8CAC;
constexpr int64_t kSRGB8Alpha8 = 0x8C43, kRGBA8 = 0x8058, kRGBA16F = 0x881A, kRGB10A2 = 0x8059, kR11G11B10F = 0x8C3A, kRGB8 = 0x8051;
using PFNTexImage3D = void(APIENTRY*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);

#ifdef _WIN32
void* GetProc(const char* name)
{
	return (void*)wglGetProcAddress(name);
}
#else
void* GetProc(const char* name)
{
	return (void*)glXGetProcAddressARB((const GLubyte*)name);
}
#endif

class GLGfx : public GfxApi {
public:
#ifdef _WIN32
	HWND window = nullptr;
	HDC dc = nullptr;
	HGLRC rc = nullptr;
#else
	Display* display = nullptr;
	XVisualInfo* visual = nullptr;
	Colormap colormap = 0;
	Window window = 0;
	GLXContext rc = nullptr;
#endif
	PFNTexImage3D texImage3D = nullptr;
	std::vector<GLuint> made;

	~GLGfx() override
	{
		if (rc && !made.empty())
			glDeleteTextures((GLsizei)made.size(), made.data());
#ifdef _WIN32
		if (rc) {
			wglMakeCurrent(nullptr, nullptr);
			wglDeleteContext(rc);
		}
		if (dc)
			ReleaseDC(window, dc);
		if (window)
			DestroyWindow(window);
#else
		if (rc) {
			glXMakeCurrent(display, 0, nullptr);
			glXDestroyContext(display, rc);
		}
		if (window)
			XDestroyWindow(display, window);
		if (colormap)
			XFreeColormap(display, colormap);
		if (visual)
			XFree(visual);
		if (display)
			XCloseDisplay(display);
#endif
	}

#ifdef _WIN32
	bool MakeContext(std::string* whyNot)
	{
		WNDCLASSA wc{};
		wc.lpfnWndProc = DefWindowProcA;
		wc.hInstance = GetModuleHandleA(nullptr);
		wc.lpszClassName = "oc_conformance_gl";
		RegisterClassA(&wc);
		window = CreateWindowA(wc.lpszClassName, "", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, nullptr, nullptr, wc.hInstance, nullptr);
		if (!window) {
			*whyNot = "no window for an OpenGL context";
			return false;
		}
		dc = GetDC(window);
		PIXELFORMATDESCRIPTOR pfd{ sizeof(pfd), 1 };
		pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
		pfd.iPixelType = PFD_TYPE_RGBA;
		pfd.cColorBits = 32;
		int format = ChoosePixelFormat(dc, &pfd);
		if (!format || !SetPixelFormat(dc, format, &pfd)) {
			*whyNot = "no OpenGL pixel format";
			return false;
		}
		rc = wglCreateContext(dc);
		if (!rc || !wglMakeCurrent(dc, rc)) {
			*whyNot = "no OpenGL context";
			return false;
		}
		return true;
	}
#else
	bool MakeContext(std::string* whyNot)
	{
		display = XOpenDisplay(nullptr);
		if (!display) {
			*whyNot = "no X display for an OpenGL context";
			return false;
		}
		// clang-format off
		const int attribs[] = {
			GLX_RENDER_TYPE, GLX_RGBA_BIT,
			GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
			GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8,
			GLX_DOUBLEBUFFER, True,
			0,
		};
		// clang-format on
		int count = 0;
		GLXFBConfig* configs = glXChooseFBConfig(display, DefaultScreen(display), attribs, &count);
		if (!configs || count < 1) {
			if (configs)
				XFree(configs);
			*whyNot = "no GLX framebuffer config";
			return false;
		}
		GLXFBConfig config = configs[0];
		XFree(configs);
		visual = glXGetVisualFromFBConfig(display, config);
		if (!visual) {
			*whyNot = "no visual for the GLX framebuffer config";
			return false;
		}
		Window root = RootWindow(display, visual->screen);
		colormap = XCreateColormap(display, root, visual->visual, AllocNone);
		XSetWindowAttributes swa{};
		swa.colormap = colormap;
		window = XCreateWindow(display, root, 0, 0, 16, 16, 0, visual->depth, InputOutput, visual->visual, CWColormap, &swa);
		rc = glXCreateNewContext(display, config, GLX_RGBA_TYPE, nullptr, True);
		if (!rc || !glXMakeCurrent(display, window, rc)) {
			*whyNot = "no OpenGL context";
			return false;
		}
		return true;
	}
#endif

	bool Init(std::string* whyNot)
	{
		if (!MakeContext(whyNot))
			return false;
#ifdef _WIN32
		const bool gl3 = GetProc("glGenFramebuffers") != nullptr;
#else
		// GLX hands out a pointer for any name, so the version says
		int major = 0;
		const char* version = (const char*)glGetString(GL_VERSION);
		const bool gl3 = version && std::sscanf(version, "%d", &major) == 1 && major >= 3;
#endif
		if (!gl3) {
			*whyNot = std::string("no OpenGL 3 driver (only ") + (const char*)glGetString(GL_RENDERER) + ")";
			return false;
		}
		texImage3D = (PFNTexImage3D)GetProc("glTexImage3D");
		return true;
	}

	GfxTexture Wrap(GLuint name)
	{
		GfxTexture g;
		g.tex.eType = vr::TextureType_OpenGL;
		g.tex.eColorSpace = vr::ColorSpace_Auto;
		g.tex.handle = (void*)(uintptr_t)name;
		g.keep = name ? (void*)(uintptr_t)name : nullptr;
		if (name)
			made.push_back(name);
		return g;
	}

	GfxTexture Pattern(uint32_t w, uint32_t h, int64_t nativeFormat, int eye, uint32_t samples, uint32_t arraySize) override
	{
		bool known = nativeFormat == kSRGB8Alpha8 || nativeFormat == kRGBA8 || nativeFormat == kRGBA16F || nativeFormat == kRGB10A2
		    || nativeFormat == kR11G11B10F || nativeFormat == kRGB8;
		arraySize = arraySize ? arraySize : 1;
		if (!known || !w || !h || samples > 1 || (arraySize > 1 && !texImage3D))
			return Wrap(0); // multisampled patterns: D3D11 only for now
		// RGBA8 bytes whatever the internal format: OpenGL converts. A float or 10-bit format holds linear values, so
		// the bytes are the colours' linear values for those.
		const bool linear = nativeFormat == kRGBA16F || nativeFormat == kRGB10A2 || nativeFormat == kR11G11B10F;
		std::vector<uint8_t> data = gfx::PatternTexels(gfx::Texels::RGBA8, w, h, eye, (size_t)w * 4, true, linear);
		GLint previous = 0;
		GLenum target = arraySize > 1 ? kTexture2DArray : GL_TEXTURE_2D;
		glGetIntegerv(arraySize > 1 ? kTextureBinding2DArray : GL_TEXTURE_BINDING_2D, &previous);
		// Earlier errors cleared (at most 32: the context reports each one once)
		for (int i = 0; i < 32 && glGetError() != GL_NO_ERROR; i++) {
		}
		GLuint tex = 0;
		glGenTextures(1, &tex);
		glBindTexture(target, tex);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(target, 0x813D /* GL_TEXTURE_MAX_LEVEL */, 0);
		if (arraySize > 1) {
			// Layer i: eye (eye + i) % 2's pattern
			std::vector<uint8_t> other = gfx::PatternTexels(gfx::Texels::RGBA8, w, h, 1 - eye, (size_t)w * 4, true, linear);
			std::vector<uint8_t> layers;
			for (uint32_t i = 0; i < arraySize; i++)
				layers.insert(layers.end(), i % 2 ? other.begin() : data.begin(), i % 2 ? other.end() : data.end());
			texImage3D(target, 0, (GLint)nativeFormat, (GLsizei)w, (GLsizei)h, (GLsizei)arraySize, 0, GL_RGBA, GL_UNSIGNED_BYTE, layers.data());
		} else {
			glTexImage2D(target, 0, (GLint)nativeFormat, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data.data());
		}
		glBindTexture(target, (GLuint)previous);
		if (glGetError() != GL_NO_ERROR) {
			glDeleteTextures(1, &tex);
			return Wrap(0);
		}
		return Wrap(tex);
	}

	GfxTexture Depth(uint32_t w, uint32_t h, float value) override
	{
		std::vector<float> data((size_t)w * h, value);
		GLint previous = 0;
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
		// Earlier errors cleared (at most 32: the context reports each one once)
		for (int i = 0; i < 32 && glGetError() != GL_NO_ERROR; i++) {
		}
		GLuint tex = 0;
		glGenTextures(1, &tex);
		glBindTexture(GL_TEXTURE_2D, tex);
		glTexImage2D(GL_TEXTURE_2D, 0, (GLint)kDepthComponent32F, (GLsizei)w, (GLsizei)h, 0, GL_DEPTH_COMPONENT, GL_FLOAT, data.data());
		glBindTexture(GL_TEXTURE_2D, (GLuint)previous);
		if (glGetError() != GL_NO_ERROR) {
			glDeleteTextures(1, &tex);
			return Wrap(0);
		}
		return Wrap(tex);
	}

	void Flush() override { glFinish(); }
};

} // namespace

std::unique_ptr<GfxApi> MakeGLGfx(std::string* whyNot)
{
	auto g = std::make_unique<GLGfx>();
	if (!g->Init(whyNot))
		return nullptr;
	return g;
}
