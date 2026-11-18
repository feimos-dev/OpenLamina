// A CpuSwapchain's D3D11 part: the image's texels go into the swapchain texture through UpdateSubresource on the
// session device's immediate context.

#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)

#include "CpuSwapchain.h"

#include "XrDriverPrivate.h"

#include <d3d11.h>
#include <dxgiformat.h>

namespace {

class CpuSwapchainD3D11 : public CpuSwapchainApi {
public:
	explicit CpuSwapchainD3D11(ID3D11Device* dev)
	    : device(dev)
	{
		device->AddRef();
		device->GetImmediateContext(&context);
	}
	~CpuSwapchainD3D11() override
	{
		DropImages();
		if (context)
			context->Release();
		device->Release();
	}

	std::vector<std::pair<int64_t, bool>> Formats() const override
	{
		return { { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, false }, { DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, true },
			{ DXGI_FORMAT_R8G8B8A8_UNORM, false }, { DXGI_FORMAT_B8G8R8A8_UNORM, true } };
	}

	bool TakeImages(XrSwapchain chain, uint32_t w, uint32_t, int64_t) override
	{
		uint32_t count = 0;
		if (XR_FAILED(xrEnumerateSwapchainImages(chain, 0, &count, nullptr)))
			return false;
		std::vector<XrSwapchainImageD3D11KHR> list(count, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
		if (XR_FAILED(xrEnumerateSwapchainImages(chain, count, &count, (XrSwapchainImageBaseHeader*)list.data())))
			return false;
		for (const XrSwapchainImageD3D11KHR& i : list)
			images.push_back(i.texture);
		width = w;
		return context != nullptr && !images.empty();
	}

	bool Write(uint32_t index, const std::vector<uint8_t>& texels) override
	{
		if (index >= images.size() || !images[index])
			return false;
		context->UpdateSubresource(images[index], 0, nullptr, texels.data(), width * 4, 0);
		return true;
	}

	void DropImages() override { images.clear(); }

private:
	ID3D11Device* device = nullptr;
	ID3D11DeviceContext* context = nullptr;
	std::vector<ID3D11Texture2D*> images; // the runtime's
	uint32_t width = 0;
};

} // namespace

std::unique_ptr<CpuSwapchainApi> MakeCpuSwapchainD3D11(const XrBaseInStructure* binding)
{
	auto* b = (const XrGraphicsBindingD3D11KHR*)binding;
	if (!b->device)
		return nullptr;
	return std::make_unique<CpuSwapchainD3D11>(b->device);
}

#endif
