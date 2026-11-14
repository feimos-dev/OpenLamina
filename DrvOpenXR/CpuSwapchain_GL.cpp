// A CpuSwapchain's OpenGL part: the image's texels go into the swapchain texture with glTexSubImage2D, in the
// session's context (made current for the call when another one is, then put back). OpenGL's rows run bottom-up, so
// the image's are written last row first.

#ifdef SUPPORT_GL

#include "CpuSwapchain.h"

#include "XrDriverPrivate.h"

#ifdef _WIN32
#include <Windows.h>
#endif
#include <GL/gl.h>
#ifndef _WIN32
#include <GL/glx.h>
#endif

#include <cstring>

namespace {

constexpr int64_t kSRGB8Alpha8 = 0x8C43; // GL_SRGB8_ALPHA8
constexpr int64_t kRGBA8 = 0x8058; // GL_RGBA8

class CpuSwapchainGL : public CpuSwapchainApi {
public:
#ifdef _WIN32
	CpuSwapchainGL(HDC dc, HGLRC rc)
	    : hDC(dc), hGLRC(rc)
	{
	}
#else
	CpuSwapchainGL(Display* d, GLXDrawable dr, GLXContext c)
	    : display(d), drawable(dr), context(c)
	{
	}
#endif

	std::vector<std::pair<int64_t, bool>> Formats() const override { return { { kSRGB8Alpha8, false }, { kRGBA8, false } }; }

	bool TakeImages(XrSwapchain chain, uint32_t w, uint32_t h, int64_t) override
	{
		uint32_t count = 0;
		if (XR_FAILED(xrEnumerateSwapchainImages(chain, 0, &count, nullptr)))
			return false;
		std::vector<XrSwapchainImageOpenGLKHR> list(count, { XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR });
		if (XR_FAILED(xrEnumerateSwapchainImages(chain, count, &count, (XrSwapchainImageBaseHeader*)list.data())))
			return false;
		for (const XrSwapchainImageOpenGLKHR& i : list)
			images.push_back(i.image);
		width = w;
		height = h;
		return !images.empty();
	}

	bool Write(uint32_t index, const std::vector<uint8_t>& texels) override
	{
		if (index >= images.size())
			return false;
		Current current(*this);
		if (!current.ok)
			return false;
		std::vector<uint8_t> bottomUp(texels.size());
		const size_t row = (size_t)width * 4;
		for (uint32_t y = 0; y < height; y++)
			std::memcpy(&bottomUp[(size_t)(height - 1 - y) * row], &texels[(size_t)y * row], row);
		for (int i = 0; i < 32 && glGetError() != GL_NO_ERROR; i++) {
		}
		GLint previous = 0, alignment = 4;
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
		glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
		glBindTexture(GL_TEXTURE_2D, images[index]);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)width, (GLsizei)height, GL_RGBA, GL_UNSIGNED_BYTE, bottomUp.data());
		glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
		glBindTexture(GL_TEXTURE_2D, (GLuint)previous);
		bool ok = glGetError() == GL_NO_ERROR;
		// Done before another context (the runtime's, or the one put back) uses the texture
		if (current.switched)
			glFinish();
		else
			glFlush();
		return ok;
	}

	void DropImages() override { images.clear(); }

private:
	// The session's context current on this thread for the scope: the one that was current is put back after
	struct Current {
		CpuSwapchainGL& gl;
		bool ok = false, switched = false;
#ifdef _WIN32
		HDC oldDC = nullptr;
		HGLRC oldRC = nullptr;
		explicit Current(CpuSwapchainGL& g)
		    : gl(g)
		{
			oldDC = wglGetCurrentDC();
			oldRC = wglGetCurrentContext();
			if (oldRC == gl.hGLRC) {
				ok = true;
				return;
			}
			ok = switched = wglMakeCurrent(gl.hDC, gl.hGLRC) != FALSE;
		}
		~Current()
		{
			if (switched)
				wglMakeCurrent(oldDC, oldRC);
		}
#else
		Display* oldDisplay = nullptr;
		GLXDrawable oldDrawable = 0;
		GLXContext oldContext = nullptr;
		explicit Current(CpuSwapchainGL& g)
		    : gl(g)
		{
			oldDisplay = glXGetCurrentDisplay();
			oldDrawable = glXGetCurrentDrawable();
			oldContext = glXGetCurrentContext();
			if (oldContext == gl.context) {
				ok = true;
				return;
			}
			ok = switched = glXMakeCurrent(gl.display, gl.drawable, gl.context) != False;
		}
		~Current()
		{
			if (switched)
				glXMakeCurrent(oldDisplay ? oldDisplay : gl.display, oldDrawable, oldContext);
		}
#endif
	};

#ifdef _WIN32
	HDC hDC;
	HGLRC hGLRC;
#else
	Display* display;
	GLXDrawable drawable;
	GLXContext context;
#endif
	std::vector<GLuint> images; // the runtime's
	uint32_t width = 0, height = 0;
};

} // namespace

std::unique_ptr<CpuSwapchainApi> MakeCpuSwapchainGL(const XrBaseInStructure* binding)
{
#ifdef _WIN32
	auto* b = (const XrGraphicsBindingOpenGLWin32KHR*)binding;
	if (!b->hGLRC)
		return nullptr;
	return std::make_unique<CpuSwapchainGL>(b->hDC, b->hGLRC);
#else
	auto* b = (const XrGraphicsBindingOpenGLXlibKHR*)binding;
	if (!b->glxContext)
		return nullptr;
	return std::make_unique<CpuSwapchainGL>(b->xDisplay, b->glxDrawable, b->glxContext);
#endif
}

#endif
