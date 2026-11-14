// A CpuSwapchain's D3D12 part: the image's texels go into an upload buffer, and from there into the swapchain image
// with CopyTextureRegion on the session's queue (waited for, at most 5 s). Swapchain images are in RENDER_TARGET
// whenever the runtime has them, as XR_KHR_D3D12_enable says.

#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)

#include "CpuSwapchain.h"

#include "XrDriverPrivate.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <cstring>

using Microsoft::WRL::ComPtr;

namespace {

class CpuSwapchainD3D12 : public CpuSwapchainApi {
public:
	CpuSwapchainD3D12(ID3D12Device* dev, ID3D12CommandQueue* q)
	    : device(dev), queue(q)
	{
	}
	~CpuSwapchainD3D12() override
	{
		DropImages();
		if (event)
			CloseHandle(event);
	}

	std::vector<std::pair<int64_t, bool>> Formats() const override
	{
		return { { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, false }, { DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, true },
			{ DXGI_FORMAT_R8G8B8A8_UNORM, false }, { DXGI_FORMAT_B8G8R8A8_UNORM, true } };
	}

	bool TakeImages(XrSwapchain chain, uint32_t w, uint32_t h, int64_t) override
	{
		uint32_t count = 0;
		if (XR_FAILED(xrEnumerateSwapchainImages(chain, 0, &count, nullptr)))
			return false;
		std::vector<XrSwapchainImageD3D12KHR> list(count, { XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR });
		if (XR_FAILED(xrEnumerateSwapchainImages(chain, count, &count, (XrSwapchainImageBaseHeader*)list.data())) || list.empty())
			return false;
		for (const XrSwapchainImageD3D12KHR& i : list)
			images.push_back(i.texture);
		width = w;
		height = h;

		D3D12_RESOURCE_DESC desc = images[0]->GetDesc();
		UINT64 total = 0;
		device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
		D3D12_HEAP_PROPERTIES upload{ D3D12_HEAP_TYPE_UPLOAD };
		D3D12_RESOURCE_DESC bd{};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bd.Width = total;
		bd.Height = 1;
		bd.DepthOrArraySize = 1;
		bd.MipLevels = 1;
		bd.SampleDesc = { 1, 0 };
		bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		if (FAILED(device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buffer)))
		    || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))
		    || FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list12)))
		    || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
			return false;
		list12->Close();
		if (!event)
			event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		return event != nullptr;
	}

	bool Write(uint32_t index, const std::vector<uint8_t>& texels) override
	{
		if (index >= images.size() || !buffer || !busyDone())
			return false;
		uint8_t* mapped = nullptr;
		D3D12_RANGE none{ 0, 0 };
		if (FAILED(buffer->Map(0, &none, (void**)&mapped)))
			return false;
		for (uint32_t y = 0; y < height; y++)
			std::memcpy(mapped + footprint.Offset + (size_t)y * footprint.Footprint.RowPitch, &texels[(size_t)y * width * 4], (size_t)width * 4);
		buffer->Unmap(0, nullptr);

		if (FAILED(allocator->Reset()) || FAILED(list12->Reset(allocator.Get(), nullptr)))
			return false;
		ID3D12Resource* image = images[index];
		D3D12_RESOURCE_BARRIER toCopy = Barrier(image, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
		list12->ResourceBarrier(1, &toCopy);
		D3D12_TEXTURE_COPY_LOCATION dst{};
		dst.pResource = image;
		dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dst.SubresourceIndex = 0;
		D3D12_TEXTURE_COPY_LOCATION src{};
		src.pResource = buffer.Get();
		src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		src.PlacedFootprint = footprint;
		list12->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
		D3D12_RESOURCE_BARRIER back = Barrier(image, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
		list12->ResourceBarrier(1, &back);
		if (FAILED(list12->Close()))
			return false;
		ID3D12CommandList* lists[] = { list12.Get() };
		queue->ExecuteCommandLists(1, lists);
		if (FAILED(queue->Signal(fence.Get(), ++fenceValue)))
			return false;
		return busyDone();
	}

	void DropImages() override
	{
		// Nothing of ours may still be running on them
		busyDone();
		images.clear();
		buffer.Reset();
		list12.Reset();
		allocator.Reset();
		fence.Reset();
		fenceValue = 0;
	}

private:
	static D3D12_RESOURCE_BARRIER Barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
	{
		D3D12_RESOURCE_BARRIER b{};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = r;
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		b.Transition.StateBefore = before;
		b.Transition.StateAfter = after;
		return b;
	}

	// Waits (at most 5 s) for the last copy: false if it's still running
	bool busyDone()
	{
		if (!fence || fence->GetCompletedValue() >= fenceValue)
			return true;
		fence->SetEventOnCompletion(fenceValue, event);
		return WaitForSingleObject(event, 5000) == WAIT_OBJECT_0;
	}

	ComPtr<ID3D12Device> device;
	ComPtr<ID3D12CommandQueue> queue;
	std::vector<ID3D12Resource*> images; // the runtime's
	uint32_t width = 0, height = 0;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
	ComPtr<ID3D12Resource> buffer;
	ComPtr<ID3D12CommandAllocator> allocator;
	ComPtr<ID3D12GraphicsCommandList> list12;
	ComPtr<ID3D12Fence> fence;
	UINT64 fenceValue = 0;
	HANDLE event = nullptr;
};

} // namespace

std::unique_ptr<CpuSwapchainApi> MakeCpuSwapchainD3D12(const XrBaseInStructure* binding)
{
	auto* b = (const XrGraphicsBindingD3D12KHR*)binding;
	if (!b->device || !b->queue)
		return nullptr;
	return std::make_unique<CpuSwapchainD3D12>(b->device, b->queue);
}

#endif
