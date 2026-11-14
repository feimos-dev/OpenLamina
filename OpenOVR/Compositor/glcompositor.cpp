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
#include "../Misc/Image.h"
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
#define GL_FRAMEBUFFER 0x8D40
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

void GLBaseCompositor::CopyToSwapchain(const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds, std::optional<XruEye>, vr::EVRSubmitFlags)
{
	if (glFunctionsMissing) {
		lastResult = XR_ERROR_RUNTIME_FAILURE;
		return;
	}
	// TODO: support array textures
	// Clear any pre-existing OpenGL errors
	while (glGetError() != GL_NO_ERROR) {
	}
	bool submitVerticallyFlipped = false;
	// Double-cast to suppress CLion warning
	auto src = (GLuint)(intptr_t)texture->handle;

	// Calculate how large the area to copy is
	GLsizei inputWidth, inputHeight, rawFormat;
	glBindTexture(GL_TEXTURE_2D, src); // Sadly even GLES3.2 doesn't have glGetTextureLevelParameteriv which takes the image directly
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &inputWidth);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &inputHeight);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &rawFormat);
	glBindTexture(GL_TEXTURE_2D, 0);

	XrRect2Di viewport;

	submitVerticallyFlipped = CalculateViewport(bounds, inputWidth, inputHeight, false, viewport);
	bool useBlit = submitVerticallyFlipped;

	CheckCreateSwapChain(viewport.extent.width, viewport.extent.height, texture->eColorSpace, rawFormat);
	if (!chain || XR_FAILED(lastResult) || images.empty()) {
		if (!XR_FAILED(lastResult))
			lastResult = XR_ERROR_RUNTIME_FAILURE;
		return;
	}
	useBlit = useBlit || createInfo.format != createInfoFormat;

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
		glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, src, 0);
		glBlitFramebuffer(
		    viewport.offset.x, viewport.offset.y, viewport.offset.x + (int)createInfo.width, viewport.offset.y + (int)createInfo.height,
		    0, submitVerticallyFlipped ? (int)createInfo.height : 0, (int)createInfo.width, submitVerticallyFlipped ? 0 : (int)createInfo.height,
		    GL_COLOR_BUFFER_BIT, GL_NEAREST);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	} else {
		glCopyImageSubData(
		    src, GL_TEXTURE_2D, 0, viewport.offset.x, viewport.offset.y, 0, // 0 == no mipmapping, next three are xyz
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

bool GLBaseCompositor::ReadbackToCpu(const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds, Image& out)
{
	// Read in the game's context, current on this thread: the whole of level 0 as RGBA8 (OpenGL converts), then the
	// bounds' area of it. Rows run bottom-up, and so do the bounds' v.
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
}

void GLBaseCompositor::InvokeCubemap(const vr::Texture_t* textures)
{
	// Not yet: SetSkyboxOverride answers RequestFailed
	OOVR_LOG_ONCE("GLCompositor::InvokeCubemap: Not yet supported!");
	lastResult = XR_ERROR_RUNTIME_FAILURE;
}

void GLBaseCompositor::CheckCreateSwapChain(int width, int height, vr::EColorSpace c_space, GLsizei rawformat)
{
	// See the comment for NormaliseFormat as to why we're doing this
	GLuint format = NormaliseFormat(c_space, rawformat);

	// Build out the info describing the swapchain we need
	XrSwapchainCreateInfo desc = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	desc.faceCount = 1;
	desc.width = width;
	desc.height = height;
	desc.format = format;
	desc.mipCount = 1; // TODO srcDesc.MipLevels;
	desc.sampleCount = 1;
	desc.arraySize = 1;
	desc.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;

	// if used fallback, make format match it to skip recreating chains every frame
	if (createInfo.format != createInfoFormat)
		desc.format = createInfo.format;

	// If the format has changed (or this is the first call), continue on to create the swapchain
	if (memcmp(&desc, &createInfo, sizeof(desc)) == 0) {
		return;
	}

	desc.format = format;
	createInfoFormat = format;

	OOVR_LOGF("Creating new OpenGL swapchain: %dx%d with format %d", width, height, format);

	// Make sure our format is supported
	uint32_t formatCount = 0;
	if (SwapchainCallFailed(xrEnumerateSwapchainFormats(xr_session.get(), 0, &formatCount, nullptr), "xrEnumerateSwapchainFormats"))
		return;
	std::vector<int64_t> formats(formatCount);
	if (SwapchainCallFailed(xrEnumerateSwapchainFormats(xr_session.get(), formatCount, &formatCount, formats.data()), "xrEnumerateSwapchainFormats"))
		return;

	if (std::count(formats.begin(), formats.end(), format) == 0) {
		OOVR_LOG("Missing format for swapchain creation, using fallback. Valid formats:");
		for (int64_t f : formats) {
			OOVR_LOGF("Valid format: %" PRIi64, f);
		}
		desc.format = NormaliseFormat(c_space, GL_RGBA8);
		if (!std::count(formats.begin(), formats.end(), desc.format))
			desc.format = GL_RGBA8;
		createInfoFormat = format;
	}

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

GLuint GLBaseCompositor::NormaliseFormat(vr::EColorSpace c_space, GLsizei rawFormat)
{
	switch (rawFormat) {
	case GL_RGBA:
		return GL_RGBA8;
	case GL_RGBA8:
		if (c_space == vr::ColorSpace_Gamma) {
			// This is a special case where the texture is using a linear color space
			// format but contains gamma-correction data from the non-linear sRGB color space.
			// If we dont specify an sRGB color space in the swapchain, the color data will be
			// handled incorrectly.  The the color output will be distorted out and the
			// gamma will be too bright.  Returning a compatible sRGB format type sets up the
			// proper conditions downstream for proper color handling
			return 35907; // GL_SRGB8_ALPHA8 (0x8C43)
		} else {
			return rawFormat; // GL_RGBA8
		}
	default:
		return rawFormat;
	}
}

#endif
