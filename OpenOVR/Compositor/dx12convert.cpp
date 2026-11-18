#include "stdafx.h"

#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)

#include "dx12convert.h"

#include <d3dcompiler.h>

#include <cstring>

Dx12Convert::Dx12Convert(ID3D12Device* dev)
    : device(dev)
{
	ID3D10Blob* v = d3d_compile_shader(fs_shader_code, "vs_fs", "vs_5_0");
	ID3D10Blob* p = d3d_compile_shader(fs_shader_code, "ps_fs", "ps_5_0");
	vs.Attach(v);
	ps.Attach(p);
	if (!vs || !ps) {
		OOVR_LOG("The D3D12 shader pass couldn't be compiled: textures that need it are left out");
		return;
	}

	// b0: eight constants (offset, scale, decodeSrgb, encodeSrgb, pad); t0: the source's view; s0: linear, clamped
	D3D12_DESCRIPTOR_RANGE range{};
	range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	range.NumDescriptors = 1;
	range.BaseShaderRegister = 0;
	range.OffsetInDescriptorsFromTableStart = 0;
	D3D12_ROOT_PARAMETER params[2]{};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[0].Constants.ShaderRegister = 0;
	params[0].Constants.Num32BitValues = 8;
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[1].DescriptorTable.NumDescriptorRanges = 1;
	params[1].DescriptorTable.pDescriptorRanges = &range;
	params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_STATIC_SAMPLER_DESC sampler{};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.MaxLOD = 0;
	sampler.ShaderRegister = 0;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_ROOT_SIGNATURE_DESC rs{};
	rs.NumParameters = 2;
	rs.pParameters = params;
	rs.NumStaticSamplers = 1;
	rs.pStaticSamplers = &sampler;
	ComPtr<ID3DBlob> blob, errors;
	if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors))
	    || FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSignature)))) {
		OOVR_LOGF("The D3D12 shader pass's root signature couldn't be made: %s", errors ? (const char*)errors->GetBufferPointer() : "");
		return;
	}
	srvStep = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	rtvStep = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	usable = EnsureSlots(4);
}

bool Dx12Convert::EnsureSlots(uint32_t slots)
{
	if (slots <= slotCount && srvHeap && rtvHeap)
		return true;
	D3D12_DESCRIPTOR_HEAP_DESC sd{};
	sd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	sd.NumDescriptors = slots;
	sd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	D3D12_DESCRIPTOR_HEAP_DESC rd{};
	rd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rd.NumDescriptors = slots;
	ComPtr<ID3D12DescriptorHeap> s, r;
	if (FAILED(device->CreateDescriptorHeap(&sd, IID_PPV_ARGS(&s))) || FAILED(device->CreateDescriptorHeap(&rd, IID_PPV_ARGS(&r)))) {
		OOVR_LOG("The D3D12 shader pass's descriptor heaps couldn't be made");
		return false;
	}
	srvHeap = s;
	rtvHeap = r;
	slotCount = slots;
	return true;
}

ID3D12PipelineState* Dx12Convert::PipelineFor(DXGI_FORMAT format)
{
	auto it = pipelines.find(format);
	if (it != pipelines.end())
		return it->second.Get();
	D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
	pd.pRootSignature = rootSignature.Get();
	pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
	pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
	pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	pd.SampleMask = UINT_MAX;
	pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	pd.RasterizerState.DepthClipEnable = TRUE;
	pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pd.NumRenderTargets = 1;
	pd.RTVFormats[0] = format;
	pd.SampleDesc = { 1, 0 };
	ComPtr<ID3D12PipelineState> made;
	if (FAILED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&made)))) {
		OOVR_LOG_ONCEF("The D3D12 shader pass has no pipeline for DXGI format %d", (int)format);
		made = nullptr;
	}
	pipelines[format] = made;
	return made.Get();
}

bool Dx12Convert::Record(ID3D12GraphicsCommandList* list, uint32_t slot, ID3D12Resource* src, DXGI_FORMAT srvFormat, uint32_t srcSlice,
    ID3D12Resource* dst, DXGI_FORMAT dstFormat, uint32_t dstWidth, uint32_t dstHeight, const Pass& pass)
{
	if (!usable || slot >= slotCount)
		return false;
	ID3D12PipelineState* pipeline = PipelineFor(dstFormat);
	if (!pipeline)
		return false;

	D3D12_CPU_DESCRIPTOR_HANDLE srv = srvHeap->GetCPUDescriptorHandleForHeapStart();
	srv.ptr += (SIZE_T)slot * srvStep;
	D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
	sv.Format = srvFormat;
	sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
	sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	sv.Texture2DArray.MostDetailedMip = 0;
	sv.Texture2DArray.MipLevels = 1;
	sv.Texture2DArray.FirstArraySlice = srcSlice;
	sv.Texture2DArray.ArraySize = 1;
	device->CreateShaderResourceView(src, &sv, srv);

	D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
	rtv.ptr += (SIZE_T)slot * rtvStep;
	D3D12_RENDER_TARGET_VIEW_DESC rv{};
	rv.Format = dstFormat;
	rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	device->CreateRenderTargetView(dst, &rv, rtv);

	float constants[8] = { pass.offsetX, pass.offsetY, pass.scaleX, pass.scaleY, pass.decodeSrgb ? 1.0f : 0.0f, pass.encodeSrgb ? 1.0f : 0.0f, 0, 0 };
	D3D12_GPU_DESCRIPTOR_HANDLE srvGpu = srvHeap->GetGPUDescriptorHandleForHeapStart();
	srvGpu.ptr += (UINT64)slot * srvStep;
	D3D12_VIEWPORT viewport{ 0, 0, (float)dstWidth, (float)dstHeight, 0, 1 };
	D3D12_RECT scissor{ 0, 0, (LONG)dstWidth, (LONG)dstHeight };

	ID3D12DescriptorHeap* heaps[] = { srvHeap.Get() };
	list->SetDescriptorHeaps(1, heaps);
	list->SetGraphicsRootSignature(rootSignature.Get());
	list->SetPipelineState(pipeline);
	list->SetGraphicsRoot32BitConstants(0, 8, constants, 0);
	list->SetGraphicsRootDescriptorTable(1, srvGpu);
	list->RSSetViewports(1, &viewport);
	list->RSSetScissorRects(1, &scissor);
	list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
	list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	list->DrawInstanced(4, 1, 0, 0);
	return true;
}

#endif
