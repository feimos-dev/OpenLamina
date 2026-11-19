#pragma once

#include "compositor.h"

// DirectX headers for use in dx*compositor.cpp

#ifdef SUPPORT_DX
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d12.h>

#include "../Misc/Image.h"

// How a DXGI format's texels lie in memory, for reading them back (Unknown for formats that can't be)
inline TexelLayout TexelLayoutOfDxgi(DXGI_FORMAT format)
{
	switch (format) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		return TexelLayout::RGBA8;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		return TexelLayout::BGRA8;
	case DXGI_FORMAT_B8G8R8X8_TYPELESS:
	case DXGI_FORMAT_B8G8R8X8_UNORM:
	case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
		return TexelLayout::BGRX8;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS:
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
		return TexelLayout::RGBA16F;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS:
	case DXGI_FORMAT_R10G10B10A2_UNORM:
		return TexelLayout::RGB10A2;
	case DXGI_FORMAT_R11G11B10_FLOAT:
		return TexelLayout::RG11B10F;
	case DXGI_FORMAT_R32_TYPELESS:
	case DXGI_FORMAT_R32_FLOAT:
	case DXGI_FORMAT_D32_FLOAT:
		return TexelLayout::R32F;
	default:
		return TexelLayout::Unknown;
	}
}

// A typed format for a typeless one (views and resolves need one): the linear member of its family
inline DXGI_FORMAT TypedDxgi(DXGI_FORMAT format)
{
	switch (format) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
		return DXGI_FORMAT_B8G8R8A8_UNORM;
	case DXGI_FORMAT_B8G8R8X8_TYPELESS:
		return DXGI_FORMAT_B8G8R8X8_UNORM;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS:
		return DXGI_FORMAT_R16G16B16A16_FLOAT;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS:
		return DXGI_FORMAT_R10G10B10A2_UNORM;
	case DXGI_FORMAT_R32_TYPELESS:
		return DXGI_FORMAT_R32_FLOAT;
	default:
		return format;
	}
}

// The shader pass of D3D11 and D3D12 (dx11compositor.cpp): vs_fs draws a quad over the target from the cbuffer's
// source area (b0: offset, scale, decodeSrgb, encodeSrgb, pad), ps_fs samples t0 (a Texture2DArray's slice 0) with s0
extern const char fs_shader_code[];
// The HLSL compiled for target, or nullptr (logged)
ID3D10Blob* d3d_compile_shader(const char* hlsl, const char* entrypoint, const char* target);

// A transition barrier of a D3D12 resource (dx12compositor.cpp)
D3D12_RESOURCE_BARRIER Dx12Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after,
    UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);

#endif