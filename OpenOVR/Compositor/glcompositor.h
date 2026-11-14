#pragma once

#include "compositor.h"

class GLBaseCompositor : public Compositor {
public:
	explicit GLBaseCompositor() = default;

	// Override
	void CopyToSwapchain(const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds, std::optional<XruEye> eye, vr::EVRSubmitFlags submitFlags) override;

	void InvokeCubemap(const vr::Texture_t* textures) override;

	bool ReadbackToCpu(const vr::Texture_t* tex, const vr::VRTextureBounds_t* bounds, Image& out) override;

protected:
	bool CopyDepth(const vr::Texture_t* depth) override;

	/**
	 * Read the runtime-created swapchain names to [images] using the GL or GLES OpenXR structs.
	 */
	virtual void ReadSwapchainImages() = 0;

	void CheckCreateSwapChain(int width, int height, vr::EColorSpace c_space, GLsizei format);


	GLuint fboId[2] = { 0 };

	// The swapchain's format isn't the texture's family: it's blitted in, which converts
	bool convertFormat = false;
	// The colour space the swapchain was made for
	vr::EColorSpace createColourSpace = vr::ColorSpace_Auto;

	std::vector<GLuint> images;
};

#ifdef SUPPORT_GL
class GLCompositor : public GLBaseCompositor {
public:
	explicit GLCompositor(GLuint initialTexture);

protected:
	void ReadSwapchainImages() override;
};
#endif
