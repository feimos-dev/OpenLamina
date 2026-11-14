#include "stdafx.h"

#if defined(SUPPORT_GL) || defined(SUPPORT_GLES)
#include "glcompositor.h"
#endif

// Import GL or GLES
#ifdef SUPPORT_GL
#include <GL/gl.h>
#endif
#ifdef SUPPORT_GLES
#include <GLES3/gl32.h>
#endif

// OpenGL compositor
#ifdef SUPPORT_GL

#include "../DrvOpenXR/XrBackend.h"
#include "../Misc/Config.h"
#include "../Misc/Image.h"
#include "../Misc/SoftFail.h"
#include "FormatTable.h"
#include <algorithm>
#include <cinttypes>
#include <string>

// On Linux these seem to already be defined
#ifdef _WIN32
typedef void(APIENTRY* PFNGLGETTEXTURELEVELPARAMETERIVPROC)(GLuint texture, GLint level, GLenum pname, GLint* params);
typedef void(APIENTRY* PFNGLCOPYIMAGESUBDATAPROC)(GLuint srcName, GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ,
    GLuint dstName, GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ, GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth);
typedef void(APIENTRY* PFNGLGENFRAMEBUFFERSPROC)(GLsizei n, GLuint* framebuffers);
typedef void(APIENTRY* PFNGLBINDFRAMEBUFFERPROC)(GLenum target, GLuint framebuffer);
typedef void(APIENTRY* PFNGLBLITFRAMEBUFFERPROC)(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter);
typedef void(APIENTRY* PFNGLFRAMEBUFFERTEXTURE2DEXTPROC)(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
typedef void(APIENTRY* PFNGLFRAMEBUFFERTEXTURELAYERPROC)(GLenum target, GLenum attachment, GLuint texture, GLint level, GLint layer);
#define GL_FRAMEBUFFER 0x8D40
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif

static PFNGLGETTEXTURELEVELPARAMETERIVPROC glGetTextureLevelParameteriv = nullptr;
// Set if the driver lacks one of the functions below (GLCompositor's constructor)
static bool glFunctionsMissing = false;
static PFNGLCOPYIMAGESUBDATAPROC glCopyImageSubData = nullptr;
static PFNGLBINDFRAMEBUFFERPROC glBindFramebuffer = nullptr;
static PFNGLGENFRAMEBUFFERSPROC glGenFramebuffers = nullptr;
static PFNGLBLITFRAMEBUFFERPROC glBlitFramebuffer = nullptr;
static PFNGLFRAMEBUFFERTEXTURE2DEXTPROC glFramebufferTexture2D = nullptr;
static PFNGLFRAMEBUFFERTEXTURELAYERPROC glFramebufferTextureLayer = nullptr;

#ifndef GL_DEPTH_ATTACHMENT
#define GL_DEPTH_ATTACHMENT 0x8D00
#endif
#ifndef GL_DRAW_FRAMEBUFFER_BINDING
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_READ_FRAMEBUFFER_BINDING
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#endif
#ifndef GL_TEXTURE_2D_ARRAY
#define GL_TEXTURE_2D_ARRAY 0x8C1A
#endif
#ifndef GL_TEXTURE_DEPTH
#define GL_TEXTURE_DEPTH 0x8071
#endif

static void* getGlProcAddr(const char* name)
{
#ifdef _WIN32
	return (void*)wglGetProcAddress(name);
#else
	return (void*)glXGetProcAddress((const GLubyte*)name);
#endif
}
GLCompositor::GLCompositor(GLuint initialTexture)
{
	if (!glGetTextureLevelParameteriv) {
		// A function the driver doesn't have: no OpenGL copies (Submit answers RequestFailed), nothing ends
#define LOAD_FUNC(x)                                    \
	if (!(*(void**)& x = (void*)getGlProcAddr(#x))) { \
		OOVR_LOG("Could not get function " #x);         \
		glFunctionsMissing = true;                      \
	}
		LOAD_FUNC(glGetTextureLevelParameteriv);
		LOAD_FUNC(glCopyImageSubData);
		LOAD_FUNC(glBindFramebuffer);
		LOAD_FUNC(glGenFramebuffers);
		LOAD_FUNC(glBlitFramebuffer);
		LOAD_FUNC(glFramebufferTexture2D);
		LOAD_FUNC(glFramebufferTextureLayer);
	}
#undef LOAD_FUNC
	if (glFunctionsMissing)
		return;
	glGenFramebuffers(2, fboId);
}

void GLCompositor::ReadSwapchainImages()
{
	// Enumerate all the swapchain images
	uint32_t imageCount = 0;
	images.clear();
	bool listed = !SwapchainCallFailed(xrEnumerateSwapchainImages(chain, 0, &imageCount, nullptr), "xrEnumerateSwapchainImages");
	auto handles = std::vector<XrSwapchainImageOpenGLKHR>(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR });
	listed = listed && !SwapchainCallFailed(xrEnumerateSwapchainImages(chain, imageCount, &imageCount, (XrSwapchainImageBaseHeader*)handles.data()), "xrEnumerateSwapchainImages");
	if (!listed || imageCount == 0) {
		// A swapchain without images can't be used: it goes, and is made again at a later Submit
		lastResult = XR_FAILED(lastResult) ? lastResult : XR_ERROR_RUNTIME_FAILURE;
		xrDestroySwapchain(chain);
		chain = XR_NULL_HANDLE;
		createInfo = {};
		return;
	}

	for (const XrSwapchainImageOpenGLKHR& img : handles) {
		images.push_back(img.image);
	}
}

#endif

// BaseGLCompositor

#if defined(SUPPORT_GL) || defined(SUPPORT_GLES)

void GLBaseCompositor::CopyToSwapchain(const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds, std::optional<XruEye> eye, vr::EVRSubmitFlags submitFlags)
{
	if (glFunctionsMissing) {
		lastResult = XR_ERROR_RUNTIME_FAILURE;
		return;
	}
	// Clear any pre-existing OpenGL errors (at most 32: the context reports each one once)
	for (int i = 0; i < 32 && glGetError() != GL_NO_ERROR; i++) {
	}
	// Double-cast to suppress CLion warning
	auto src = (GLuint)(intptr_t)texture->handle;

	// An array texture (Submit_GlArrayTexture, or SubmitWithArrayIndex) shows a slice: SubmitWithArrayIndex's, else the
	// eye's. A plain texture given to SubmitWithArrayIndex with slice 0 is taken as it is.
	bool array = (submitFlags & vr::Submit_GlArrayTexture) || submitSlice.has_value();
	const GLint layer = array ? (GLint)(submitSlice ? *submitSlice : (uint32_t)eye.value_or(XruEyeLeft)) : 0;

	// Calculate how large the area to copy is
	GLsizei inputWidth = 0, inputHeight = 0, rawFormat = 0, layers = 1;
	GLenum target = array ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
	auto query = [&]() {
		glBindTexture(target, src); // Sadly even GLES3.2 doesn't have glGetTextureLevelParameteriv which takes the image directly
		glGetTexLevelParameteriv(target, 0, GL_TEXTURE_WIDTH, &inputWidth);
		glGetTexLevelParameteriv(target, 0, GL_TEXTURE_HEIGHT, &inputHeight);
		glGetTexLevelParameteriv(target, 0, GL_TEXTURE_INTERNAL_FORMAT, &rawFormat);
		if (array)
			glGetTexLevelParameteriv(target, 0, GL_TEXTURE_DEPTH, &layers);
		glBindTexture(target, 0);
	};
	query();
	if (array && inputWidth <= 0 && layer == 0) {
		for (int i = 0; i < 32 && glGetError() != GL_NO_ERROR; i++) {
		}
		array = false;
		target = GL_TEXTURE_2D;
		layers = 1;
		query();
	}
	if (inputWidth <= 0 || inputHeight <= 0 || layer >= layers) {
		// Submit answers InvalidTexture; the eye is left out
		lastResult = XR_ERROR_VALIDATION_FAILURE;
		OC_UNSUPPORTED_VOID("An OpenGL texture that can't be used: %dx%d, slice %d of %d", inputWidth, inputHeight, layer, layers);
	}

	// The bounds' area (in OpenGL's rows, bottom-up), which the swapchain is the size of
	TextureRegion area;
	if (!RegionFromBounds(bounds, (uint32_t)inputWidth, (uint32_t)inputHeight, area)) {
		// Submit answers InvalidTexture; the eye is left out
		lastResult = XR_ERROR_VALIDATION_FAILURE;
		OC_UNSUPPORTED_VOID("OpenGL texture whose bounds have no area: u %f to %f, v %f to %f", (double)Bounds(bounds).uMin, (double)Bounds(bounds).uMax, (double)Bounds(bounds).vMin, (double)Bounds(bounds).vMax);
	}
	// The blit flips where the bounds run backwards (invertUsingShaders: always used)
	bool useBlit = area.flipX || area.flipY || oovr_global_configuration.InvertUsingShaders();

	CheckCreateSwapChain((int)area.Width(), (int)area.Height(), texture->eColorSpace, rawFormat);
	if (!chain || XR_FAILED(lastResult) || images.empty()) {
		if (!XR_FAILED(lastResult))
			lastResult = XR_ERROR_RUNTIME_FAILURE;
		return;
	}
	// A swapchain format that isn't of the texture's family is converted by the blit
	useBlit = useBlit || convertFormat;

	// Reserve an image from the swapchain and wait until the compositor is done with it (a timed-out wait is
	// picked up again at the next Submit)
	uint32_t currentIndex = 0;
	if (!AcquireAndWait((uint32_t)images.size(), currentIndex))
		return;

	// Actually copy the image across
	GLuint dst = images.at(currentIndex);
	if (useBlit) {
		glBindFramebuffer(GL_FRAMEBUFFER, fboId[1]);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst, 0);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, fboId[0]);
		if (array)
			glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, src, 0, layer);
		else
			glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, src, 0);
		// Reversed source coordinates flip
		glBlitFramebuffer(
		    (GLint)(area.flipX ? area.x1 : area.x0), (GLint)(area.flipY ? area.y1 : area.y0), (GLint)(area.flipX ? area.x0 : area.x1), (GLint)(area.flipY ? area.y0 : area.y1),
		    0, 0, (int)createInfo.width, (int)createInfo.height,
		    GL_COLOR_BUFFER_BIT, GL_NEAREST);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	} else {
		glCopyImageSubData(
		    src, target, 0, (GLint)area.x0, (GLint)area.y0, layer, // 0 == no mipmapping, next three are xyz
		    dst, GL_TEXTURE_2D, 0, 0, 0, 0, // Same as above but for the destination
		    (int)createInfo.width, (int)createInfo.height, 1 // Region of the output texture to copy into (in this case, everything)
		);
	}

	// Abort if there was an OpenGL error
	GLenum err = glGetError();
	if (err != GL_NO_ERROR) {
		// Just warn, so we can find it in the logs - I fear it may crash in unexpected corner-cases otherwise.
		// OOVR_ABORTF("OpenGL texture copy failed with err %d", err);
		OOVR_LOG_ONCE("WARNING: OpenGL texture copy failed!");
	}

#if defined(SUPPORT_GL) && !defined(_WIN32)
	const auto binding = (XrGraphicsBindingOpenGLXlibKHR*)((XrBackend*)BackendManager::Instance().GetBackendInstance())->GetCurrentGraphicsBinding();
	if (binding->type == XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR && binding->glxContext != glXGetCurrentContext())
		glFinish();
#endif

	// Release the swapchain - OpenXR will use the last-released image in a swapchain
	XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	SwapchainCallFailed(xrReleaseSwapchainImage(chain, &releaseInfo), "xrReleaseSwapchainImage");
}

bool GLBaseCompositor::CopyDepth(const vr::Texture_t* depth)
{
	if (glFunctionsMissing)
		return false;
	auto src = (GLuint)(intptr_t)depth->handle;
	for (int i = 0; i < 32 && glGetError() != GL_NO_ERROR; i++) {
	}
	GLint previous = 0, width = 0, height = 0, internalFormat = 0;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
	glBindTexture(GL_TEXTURE_2D, src);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &internalFormat);
	glBindTexture(GL_TEXTURE_2D, (GLuint)previous);
	if (width <= 0 || height <= 0)
		return false;
	if (!chain || (int)createInfo.width != width || (int)createInfo.height != height || createInfoFormat != internalFormat) {
		std::optional<int64_t> format = ChooseDepthSwapchainFormat(GfxApiKind::OpenGL, internalFormat, RuntimeFormats());
		if (!format) {
			OOVR_LOG_ONCEF("No depth swapchain format of the runtime's for an OpenGL depth texture of internal format 0x%x: depth not sent", (unsigned)internalFormat);
			return false;
		}
		if (!MakeDepthChain(*format, (uint32_t)width, (uint32_t)height))
			return false;
		createInfoFormat = internalFormat;
		ReadSwapchainImages();
		if (!chain || images.empty())
			return false;
	}
	uint32_t index = 0;
	if (!AcquireAndWait((uint32_t)images.size(), index))
		return false;
	// A depth texture is blitted whole (level 0), as its framebuffer's depth: unlike glCopyImageSubData, that doesn't
	// need the texture to be complete, which a depth texture without filtering set up isn't. The game's framebuffers
	// are put back.
	GLint previousDraw = 0, previousRead = 0;
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDraw);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousRead);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, fboId[0]);
	glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, src, 0);
	glReadBuffer(GL_NONE);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fboId[1]);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, images[index], 0);
#ifdef SUPPORT_GL
	glDrawBuffer(GL_NONE);
#else
	const GLenum noDrawBuffer = GL_NONE;
	glDrawBuffers(1, &noDrawBuffer);
#endif
	glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)previousDraw);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)previousRead);
	if (glGetError() != GL_NO_ERROR)
		OOVR_LOG_ONCE("WARNING: OpenGL depth texture copy failed!");
	XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	return !SwapchainCallFailed(xrReleaseSwapchainImage(chain, &releaseInfo), "xrReleaseSwapchainImage (depth)");
}

bool GLBaseCompositor::ReadbackToCpu(const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds, Image& out)
{
	// Read in the game's context, current on this thread: the whole of level 0 as RGBA8 (OpenGL converts), then the
	// bounds' area of it. Rows run bottom-up, and so do the bounds' v. (OpenGL ES has no glGetTexImage.)
#ifndef SUPPORT_GL
	return Compositor::ReadbackToCpu(texture, bounds, out);
#else
	auto name = (GLuint)(intptr_t)texture->handle;
	if (!name)
		return false;
	while (glGetError() != GL_NO_ERROR) {
	}
	GLint previous = 0, width = 0, height = 0, internalFormat = 0;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
	glBindTexture(GL_TEXTURE_2D, name);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &internalFormat);
	TextureRegion region;
	if (width <= 0 || height <= 0 || !RegionFromBounds(bounds, (uint32_t)width, (uint32_t)height, region)) {
		glBindTexture(GL_TEXTURE_2D, (GLuint)previous);
		return false;
	}
	std::vector<uint8_t> texels((size_t)width * height * 4);
	GLint alignment = 4;
	glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels.data());
	glPixelStorei(GL_PACK_ALIGNMENT, alignment);
	glBindTexture(GL_TEXTURE_2D, (GLuint)previous);
	if (glGetError() != GL_NO_ERROR) {
		OOVR_LOG_ONCEF("An OpenGL texture of internal format 0x%x couldn't be read back into memory", internalFormat);
		return false;
	}
	const uint8_t* first = &texels[((size_t)region.y0 * width + region.x0) * 4];
	out = DecodeTexels(TexelLayout::RGBA8, first, region.Width(), region.Height(), (size_t)width * 4, true, false);
	// Float and 10-bit textures hold linear values (GL_RGBA16F, GL_RGBA32F, GL_R11F_G11F_B10F, GL_RGB10_A2)
	const bool wide = internalFormat == 0x881A || internalFormat == 0x8814 || internalFormat == 0x8C3A || internalFormat == 0x8059;
	if (wide && texture->eColorSpace != vr::ColorSpace_Gamma)
		EncodeSrgbInPlace(out);
	// Flipped bounds' v counts from the bottom, so a flipped image is upright when read top-down
	FlipImage(out, region.flipX, region.flipY);
	return !out.Empty();
#endif
}

void GLBaseCompositor::InvokeCubemap(const vr::Texture_t* textures)
{
	// Not yet: SetSkyboxOverride answers RequestFailed
	OOVR_LOG_ONCE("GLCompositor::InvokeCubemap: Not yet supported!");
	lastResult = XR_ERROR_RUNTIME_FAILURE;
}

void GLBaseCompositor::CheckCreateSwapChain(int width, int height, vr::EColorSpace c_space, GLsizei rawformat)
{
	// The same texture as last time: the swapchain is kept
	if (chain && width == (int)createInfo.width && height == (int)createInfo.height && rawformat == createInfoFormat && c_space == createColourSpace)
		return;

	// The runtime's format for it: its own, or one it's blitted into (glBlitFramebuffer converts)
	std::optional<FormatChoice> choice = ChooseSwapchainFormat(GfxApiKind::OpenGL, rawformat, c_space, RuntimeFormats());
	if (!choice) {
		// Submit answers TextureUsesUnsupportedFormat; the eye is left out
		lastResult = XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
		OC_UNSUPPORTED_VOID("An OpenGL texture of internal format 0x%x: no swapchain format can show it", (unsigned)rawformat);
	}

	// Build out the info describing the swapchain we need
	XrSwapchainCreateInfo desc = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	desc.faceCount = 1;
	desc.width = width;
	desc.height = height;
	desc.format = choice->swapchainFormat;
	desc.mipCount = 1; // TODO srcDesc.MipLevels;
	desc.sampleCount = 1;
	desc.arraySize = 1;
	desc.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;

	OOVR_LOGF("Creating new OpenGL swapchain: %dx%d with format 0x%x for internal format 0x%x%s", width, height, (unsigned)desc.format,
	    (unsigned)rawformat, choice->convert ? " (blitted, which converts)" : "");
	convertFormat = choice->convert;
	createInfoFormat = rawformat;
	createColourSpace = c_space;

	// Delete the old swapchain, if applicable
	if (chain) {
		xrDestroySwapchain(chain);
		chain = XR_NULL_HANDLE;
	}

	// Create the new one
	createInfo = desc;

	ForgetAcquiredImage(); // a new swapchain: no image of the old one is pending
	if (SwapchainCallFailed(xrCreateSwapchain(xr_session.get(), &desc, &chain), "xrCreateSwapchain")) {
		chain = XR_NULL_HANDLE; // tried again at the next Submit
		createInfo = {};
		return;
	}

	// Enumerate all the swapchain images
	ReadSwapchainImages();
}

#endif
