#pragma once
// D3D12's shader pass: draws an area of a texture over the whole of a swapchain image, converting between their formats
// on the way, and flipping when the area runs backwards (bounds with uMin > uMax or vMin > vMax). Its shaders are the
// D3D11 pass's, compiled with D3DCompile at first use.

#include "dxcompositor.h"

#include <map>

#ifdef SUPPORT_DX12

class Dx12Convert {
public:
	explicit Dx12Convert(ID3D12Device* device);
	// False if its shaders, root signature or descriptor heaps couldn't be made (logged once)
	bool Usable() const { return usable; }

	struct Pass {
		// The source area: uv = offset + scale * (0..1 across the image)
		float offsetX = 0, offsetY = 0, scaleX = 1, scaleY = 1;
		bool decodeSrgb = false, encodeSrgb = false;
	};

	// Records the pass into list: src (in PIXEL_SHADER_RESOURCE, read through a view of srvFormat, slice srcSlice) over
	// dstWidth x dstHeight from the origin of dst (in RENDER_TARGET, a render target view of dstFormat), with slot's
	// descriptors (one per swapchain image, so a slot isn't rewritten while the GPU may still read it). False if it
	// couldn't.
	bool Record(ID3D12GraphicsCommandList* list, uint32_t slot, ID3D12Resource* src, DXGI_FORMAT srvFormat, uint32_t srcSlice,
	    ID3D12Resource* dst, DXGI_FORMAT dstFormat, uint32_t dstWidth, uint32_t dstHeight, const Pass& pass);

	// Makes room for slots descriptors: only while none of the pass's work is in flight
	bool EnsureSlots(uint32_t slots);

private:
	ID3D12PipelineState* PipelineFor(DXGI_FORMAT format);

	ComPtr<ID3D12Device> device;
	bool usable = false;
	ComPtr<ID3DBlob> vs, ps;
	ComPtr<ID3D12RootSignature> rootSignature;
	std::map<DXGI_FORMAT, ComPtr<ID3D12PipelineState>> pipelines;
	ComPtr<ID3D12DescriptorHeap> srvHeap, rtvHeap;
	UINT srvStep = 0, rtvStep = 0;
	uint32_t slotCount = 0;
};

#endif
