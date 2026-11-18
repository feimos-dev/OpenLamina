#include "stdafx.h"

#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)

#include "dx11compositor.h"

#include "FormatTable.h"

#include "../Misc/Config.h"
#include "../Misc/SoftFail.h"
#include "../Misc/xr_ext.h"

#include <d3dcompiler.h> // For compiling shaders! D3DCompile

#pragma comment(lib, "d3dcompiler.lib")
// D3D11CreateDevice, for the D3D10 bridge, the OpenGL and Vulkan interop and the temporary device
#pragma comment(lib, "d3d11.lib")

struct ShaderConstData {
	float offsetX, offsetY;
	float scaleX, scaleY;
	float decodeSrgb, encodeSrgb;
	float pad[2];
};

extern const char fs_shader_code[] = R"_(
Texture2DArray shaderTexture : register(t0);

SamplerState SampleType : register(s0);

cbuffer ConstData : register(b0) {
	float2 offset;
	float2 scale;
	float decodeSrgb;
	float encodeSrgb;
	float2 pad;
};

struct psIn {
	float4 pos : SV_POSITION;
	float2 tex : TEXCOORD0;
};

psIn vs_fs(uint vI : SV_VERTEXID)
{
	psIn output;
	// offset and scale come from the submitted bounds: vMin > vMax inverts the image, and a uMin/uMax
	// half picks one eye of a side-by-side texture.
	float2 basePos = float2(vI&1,vI>>1);
	output.tex = offset + basePos * scale;
	output.pos = float4((basePos.x-0.5f)*2,-(basePos.y-0.5f)*2,0,1);
	return output;
}

float4 ps_fs(psIn inputPS) : SV_TARGET
{
	float4 c = shaderTexture.Sample(SampleType, float3(inputPS.tex, 0));
	// Gamma-encoded values read through a view that doesn't decode them, into an sRGB target that encodes: decoded
	// here, so the target gets the game's values. And the other way round for a linear target.
	if (decodeSrgb > 0.5f)
		c.rgb = c.rgb <= 0.04045f ? c.rgb / 12.92f : pow(abs((c.rgb + 0.055f) / 1.055f), 2.4f);
	if (encodeSrgb > 0.5f)
		c.rgb = c.rgb <= 0.0031308f ? c.rgb * 12.92f : 1.055f * pow(abs(c.rgb), 1.0f / 2.4f) - 0.055f;
	return c;
})_";

ID3DBlob* d3d_compile_shader(const char* hlsl, const char* entrypoint, const char* target)
{
	DWORD flags = D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR | D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS;
#ifdef _DEBUG
	flags |= D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_DEBUG;
#else
	flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

	// nullptr if it doesn't compile: the compositor then copies without the shader (inverted images stay inverted)
	ID3DBlob *compiled = nullptr, *errors = nullptr;
	if (FAILED(D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, entrypoint, target, flags, 0, &compiled, &errors))) {
		OOVR_LOGF("Error: D3DCompile failed %s", errors ? (char*)errors->GetBufferPointer() : "(no message)");
		if (compiled)
			compiled->Release();
		compiled = nullptr;
	}
	if (errors)
		errors->Release();

	return compiled;
}

ID3D11RenderTargetView* d3d_make_rtv(ID3D11Device* d3d_device, XrBaseInStructure& swapchain_img, const DXGI_FORMAT& format)
{
	ID3D11RenderTargetView* result = nullptr;

	// Get information about the swapchain image that OpenXR made for us
	XrSwapchainImageD3D11KHR& d3d_swapchain_img = (XrSwapchainImageD3D11KHR&)swapchain_img;

	// Create a render target view resource for the swapchain image
	D3D11_RENDER_TARGET_VIEW_DESC target_desc = {};
	target_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	target_desc.Format = format;
	target_desc.Texture2D.MipSlice = 0;
	// nullptr if it can't be made: the shader path is then off for this swapchain
	if (FAILED(d3d_device->CreateRenderTargetView(d3d_swapchain_img.texture, &target_desc, &result)))
		result = nullptr;

	return result;
}

DX11Compositor::DX11Compositor(ID3D11Texture2D* initial)
{
	initial->GetDevice(&device);
	device->GetImmediateContext(&context);

	// Shaders for inverting copy. Anything here that fails turns the shader path off (shadersUsable), and textures
	// are then copied as they are.
	ID3DBlob* fs_vert_shader_blob = d3d_compile_shader(fs_shader_code, "vs_fs", "vs_5_0");
	ID3DBlob* fs_pixel_shader_blob = d3d_compile_shader(fs_shader_code, "ps_fs", "ps_5_0");
	shadersUsable = fs_vert_shader_blob && fs_pixel_shader_blob;
	if (shadersUsable && FAILED(device->CreateVertexShader(fs_vert_shader_blob->GetBufferPointer(), fs_vert_shader_blob->GetBufferSize(), nullptr, &fs_vshader)))
		shadersUsable = false;
	if (shadersUsable && FAILED(device->CreatePixelShader(fs_pixel_shader_blob->GetBufferPointer(), fs_pixel_shader_blob->GetBufferSize(), nullptr, &fs_pshader)))
		shadersUsable = false;
	if (fs_vert_shader_blob)
		fs_vert_shader_blob->Release();
	if (fs_pixel_shader_blob)
		fs_pixel_shader_blob->Release();

	// Create a texture sampler state description.
	D3D11_SAMPLER_DESC samplerDesc;
	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.MipLODBias = 0.0f;
	samplerDesc.MaxAnisotropy = 4;
	samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
	samplerDesc.BorderColor[0] = 0;
	samplerDesc.BorderColor[1] = 0;
	samplerDesc.BorderColor[2] = 0;
	samplerDesc.BorderColor[3] = 0;
	samplerDesc.MinLOD = 0;
	samplerDesc.MaxLOD = 0;

	// Create the texture sampler state.
	if (FAILED(device->CreateSamplerState(&samplerDesc, &quad_sampleState)))
		shadersUsable = false;

	// Create a constant buffer, to store the source area of the texture
	D3D11_BUFFER_DESC constantBufferDesc = {};
	constantBufferDesc.ByteWidth = sizeof(ShaderConstData);
	constantBufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	constantBufferDesc.Usage = D3D11_USAGE_DYNAMIC;
	constantBufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (FAILED(device->CreateBuffer(&constantBufferDesc, nullptr, &constBuffer)))
		shadersUsable = false;
	if (!shadersUsable)
		OOVR_LOG("The D3D11 compositor's shaders couldn't be set up: inverted textures are copied as they are");

	// A pipeline state of its own, swapped in for the shader pass so the game's is left as it was (D3D11.1; without
	// it the state the pass changes is saved and put back one piece at a time)
	ID3D11Device1* device1 = nullptr;
	if (SUCCEEDED(device->QueryInterface(__uuidof(ID3D11Device1), (void**)&device1)) && device1) {
		D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
		UINT flags = (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
		if (FAILED(device1->CreateDeviceContextState(flags, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &ownState)))
			ownState = nullptr;
		device1->Release();
	}
	if (ownState && FAILED(context->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&context1)))
		context1 = nullptr;
}

DX11Compositor::~DX11Compositor()
{
	for (auto&& rtv : swapchain_rtvs)
		if (rtv)
			rtv->Release();

	swapchain_rtvs.clear();

	for (auto&& tex : resolvedMSAATextures)
		tex->Release();

	resolvedMSAATextures.clear();

	if (shaderSource)
		shaderSource->Release();
	if (constBuffer)
		constBuffer->Release();
	if (context1)
		context1->Release();
	if (ownState)
		ownState->Release();

	context->Release();
	device->Release();
}

void DX11Compositor::CheckCreateSwapChain(const vr::Texture_t* texture, uint32_t width, uint32_t height, bool cube)
{
	XrSwapchainCreateInfo& desc = createInfo;

	auto* src = (ID3D11Texture2D*)texture->handle;

	D3D11_TEXTURE2D_DESC srcDesc;
	src->GetDesc(&srcDesc);
	const D3D11_TEXTURE2D_DESC fullSrcDesc = srcDesc;

	// The swapchain's size, if not the texture's
	if (width && height) {
		srcDesc.Width = width;
		srcDesc.Height = height;
	}

	if (cube) {
		// LibOVR can only use square cubemaps, while SteamVR can use any shape
		// Note we use CopySubresourceRegion later on, so this won't cause problems with that
		srcDesc.Height = srcDesc.Width = std::min(srcDesc.Height, srcDesc.Width);
	}

	bool usable = chain == NULL ? false : CheckChainCompatible(srcDesc, texture->eColorSpace);

	if (!usable) {
		OOVR_LOG("Generating new swap chain");

		OOVR_LOGF("Texture desc format: %d", srcDesc.Format);
		OOVR_LOGF("Texture desc bind flags: %d", srcDesc.BindFlags);
		OOVR_LOGF("Texture desc MiscFlags: %d", srcDesc.MiscFlags);
		OOVR_LOGF("Texture desc Usage: %d", srcDesc.Usage);
		OOVR_LOGF("Texture desc width: %d", srcDesc.Width);
		OOVR_LOGF("Texture desc height: %d", srcDesc.Height);

		// First, delete the old chain if necessary
		if (chain) {
			xrDestroySwapchain(chain);
			chain = XR_NULL_HANDLE;
		}

		for (auto&& rtv : swapchain_rtvs)
			if (rtv)
				rtv->Release();

		swapchain_rtvs.clear();

		for (auto&& tex : resolvedMSAATextures)
			tex->Release();

		resolvedMSAATextures.clear();

		// The runtime's format for it: its own, or one it's converted into (by the shader pass)
		std::optional<FormatChoice> choice = ChooseSwapchainFormat(GfxApiKind::D3D11, srcDesc.Format, texture->eColorSpace, RuntimeFormats());
		if (!choice) {
			// Submit answers TextureUsesUnsupportedFormat; the eye is left out
			createInfo = {};
			lastResult = XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
			OC_UNSUPPORTED_VOID("A D3D11 texture of DXGI format %d: no swapchain format can show it", (int)srcDesc.Format);
		}
		const DXGI_FORMAT type = (DXGI_FORMAT)choice->swapchainFormat;
		convertFormat = choice->convert;
		if (convertFormat)
			OOVR_LOGF("DXGI format %d goes into swapchain format %d through the shader pass", (int)srcDesc.Format, (int)type);

		// Set aside the old format for checking later
		createInfoFormat = srcDesc.Format;
		createColourSpace = texture->eColorSpace;

		// Make eye render buffer
		desc = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
		// TODO desc.Type = cube ? ovrTexture_Cube : ovrTexture_2D;
		desc.faceCount = cube ? 6 : 1;
		desc.width = srcDesc.Width;
		desc.height = srcDesc.Height;
		desc.format = type;
		// The copy and the shader pass write mip 0 only
		desc.mipCount = 1;
		desc.sampleCount = 1;
		desc.arraySize = 1;
		desc.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;

		ForgetAcquiredImage(); // a new swapchain: no image of the old one is pending
		if (SwapchainCallFailed(xrCreateSwapchain(xr_session.get(), &desc, &chain), "xrCreateSwapchain")) {
			chain = XR_NULL_HANDLE; // tried again at the next Submit
			return;
		}

		// Go through the images and retrieve them - this will be used later in Invoke, since OpenXR doesn't
		// have a convenient way to request one specific image.
		uint32_t imageCount = 0;
		if (SwapchainCallFailed(xrEnumerateSwapchainImages(chain, 0, &imageCount, nullptr), "xrEnumerateSwapchainImages")) {
			xrDestroySwapchain(chain);
			chain = XR_NULL_HANDLE;
			return;
		}

		imagesHandles = std::vector<XrSwapchainImageD3D11KHR>(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
		if (SwapchainCallFailed(xrEnumerateSwapchainImages(chain, imagesHandles.size(), &imageCount, (XrSwapchainImageBaseHeader*)imagesHandles.data()), "xrEnumerateSwapchainImages")
		    || imageCount != imagesHandles.size()) {
			lastResult = XR_FAILED(lastResult) ? lastResult : XR_ERROR_RUNTIME_FAILURE;
			imagesHandles.clear();
			xrDestroySwapchain(chain);
			chain = XR_NULL_HANDLE;
			return;
		}

		swapchain_rtvs.resize(imageCount, nullptr);

		for (uint32_t i = 0; i < imageCount; i++) {
			swapchain_rtvs[i] = d3d_make_rtv(device, (XrBaseInStructure&)imagesHandles[i], type);
		}

		// TODO do we need to release the images at some point, or does the swapchain do that for us?
	}

	// The resolve textures follow the source's full size and format, which can change while the swapchain fits
	if (fullSrcDesc.SampleDesc.Count > 1 && !convertFormat) {
		bool current = resolvedMSAATextures.size() == imagesHandles.size();
		if (current) {
			D3D11_TEXTURE2D_DESC have;
			resolvedMSAATextures[0]->GetDesc(&have);
			current = have.Width == fullSrcDesc.Width && have.Height == fullSrcDesc.Height && have.Format == fullSrcDesc.Format;
		}

		if (!current) {
			OOVR_LOGF("Creating resolver textures for MSAA source with sample count x%d", fullSrcDesc.SampleDesc.Count);
			for (auto&& tex : resolvedMSAATextures)
				tex->Release();

			// ResolveSubresource needs the source's full size; one slice and mip is all it resolves into
			D3D11_TEXTURE2D_DESC resDesc = fullSrcDesc;
			resDesc.SampleDesc = { 1, 0 };
			resDesc.ArraySize = 1;
			resDesc.MipLevels = 1;
			resDesc.BindFlags = 0;
			resDesc.CPUAccessFlags = 0;
			resDesc.MiscFlags = 0;

			resolvedMSAATextures.assign(imagesHandles.size(), nullptr);
			for (auto&& tex : resolvedMSAATextures) {
				if (FAILED(device->CreateTexture2D(&resDesc, nullptr, &tex))) {
					// Without them the multisampled texture can't be copied: this eye is left out
					OOVR_LOG("Couldn't make the textures to resolve a multisampled texture into");
					for (auto&& made : resolvedMSAATextures)
						if (made)
							made->Release();
					resolvedMSAATextures.clear();
					lastResult = XR_ERROR_RUNTIME_FAILURE;
					return;
				}
			}
		}
	}
}

ID3D11Texture2D* DX11Compositor::ShaderSource(const D3D11_TEXTURE2D_DESC& of, DXGI_FORMAT format)
{
	if (shaderSource) {
		D3D11_TEXTURE2D_DESC have;
		shaderSource->GetDesc(&have);
		if (have.Width == of.Width && have.Height == of.Height && have.Format == format)
			return shaderSource;
		shaderSource->Release();
		shaderSource = nullptr;
	}
	D3D11_TEXTURE2D_DESC desc = of;
	desc.Format = format;
	desc.SampleDesc = { 1, 0 };
	desc.ArraySize = 1;
	desc.MipLevels = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.CPUAccessFlags = 0;
	desc.MiscFlags = 0;
	if (FAILED(device->CreateTexture2D(&desc, nullptr, &shaderSource)))
		shaderSource = nullptr;
	return shaderSource;
}

void DX11Compositor::DrawWithShader(ID3D11ShaderResourceView* view, uint32_t index, const ShaderPass& pass)
{
	ID3DDeviceContextState* appState = nullptr;
	if (context1 && ownState)
		context1->SwapDeviceContextState(ownState, &appState);

	// Without a state of its own: what the pass changes is kept and put back
	UINT numViewPorts = 0, numScissors = 0;
	std::vector<D3D11_VIEWPORT> viewports;
	std::vector<D3D11_RECT> scissors;
	ComPtr<ID3D11RasterizerState> appRasterizer;
	ComPtr<ID3D11Buffer> appVsBuffer, appPsBuffer;
	D3D11_PRIMITIVE_TOPOLOGY currTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
	ID3D11RenderTargetView* appTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
	ComPtr<ID3D11DepthStencilView> appDepthView;
	ComPtr<ID3D11BlendState> appBlend;
	FLOAT appBlendFactor[4] = {};
	UINT appSampleMask = 0xffffffff;
	ComPtr<ID3D11DepthStencilState> appDepthState;
	UINT appStencilRef = 0;
	ComPtr<ID3D11VertexShader> appVs;
	ComPtr<ID3D11PixelShader> appPs;
	ID3D11ClassInstance* appVsInstances[D3D11_SHADER_MAX_INTERFACES] = {};
	ID3D11ClassInstance* appPsInstances[D3D11_SHADER_MAX_INTERFACES] = {};
	UINT appVsInstanceCount = D3D11_SHADER_MAX_INTERFACES, appPsInstanceCount = D3D11_SHADER_MAX_INTERFACES;
	ComPtr<ID3D11ShaderResourceView> appView;
	ComPtr<ID3D11SamplerState> appSampler;
	ComPtr<ID3D11InputLayout> appLayout;
	// The game's geometry and tessellation shaders, which would otherwise run in the pass
	ComPtr<ID3D11GeometryShader> appGs;
	ComPtr<ID3D11HullShader> appHs;
	ComPtr<ID3D11DomainShader> appDs;
	ID3D11ClassInstance* appGsInstances[D3D11_SHADER_MAX_INTERFACES] = {};
	ID3D11ClassInstance* appHsInstances[D3D11_SHADER_MAX_INTERFACES] = {};
	ID3D11ClassInstance* appDsInstances[D3D11_SHADER_MAX_INTERFACES] = {};
	UINT appGsInstanceCount = D3D11_SHADER_MAX_INTERFACES, appHsInstanceCount = D3D11_SHADER_MAX_INTERFACES,
	     appDsInstanceCount = D3D11_SHADER_MAX_INTERFACES;
	if (!appState) {
		context->RSGetViewports(&numViewPorts, nullptr);
		viewports.resize(numViewPorts);
		context->RSGetViewports(&numViewPorts, viewports.data());
		context->RSGetScissorRects(&numScissors, nullptr);
		scissors.resize(numScissors);
		context->RSGetScissorRects(&numScissors, scissors.data());
		context->RSGetState(&appRasterizer);
		context->VSGetConstantBuffers(0, 1, &appVsBuffer);
		context->PSGetConstantBuffers(0, 1, &appPsBuffer);
		context->IAGetPrimitiveTopology(&currTopology);
		context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, appTargets, &appDepthView);
		context->OMGetBlendState(&appBlend, appBlendFactor, &appSampleMask);
		context->OMGetDepthStencilState(&appDepthState, &appStencilRef);
		context->VSGetShader(&appVs, appVsInstances, &appVsInstanceCount);
		context->PSGetShader(&appPs, appPsInstances, &appPsInstanceCount);
		context->PSGetShaderResources(0, 1, &appView);
		context->PSGetSamplers(0, 1, &appSampler);
		context->IAGetInputLayout(&appLayout);
		context->GSGetShader(&appGs, appGsInstances, &appGsInstanceCount);
		context->HSGetShader(&appHs, appHsInstances, &appHsInstanceCount);
		context->DSGetShader(&appDs, appDsInstances, &appDsInstanceCount);
		context->GSSetShader(nullptr, nullptr, 0);
		context->HSSetShader(nullptr, nullptr, 0);
		context->DSSetShader(nullptr, nullptr, 0);
	}

	context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
	context->OMSetDepthStencilState(nullptr, 0);
	context->RSSetState(nullptr);

	// Draw over the pass's area from the image's origin; the shader picks the source area
	D3D11_VIEWPORT viewport = CD3D11_VIEWPORT(0.0f, 0.0f, (float)pass.width, (float)pass.height);
	context->RSSetViewports(1, &viewport);
	context->RSSetScissorRects(0, nullptr);

	context->OMSetRenderTargets(1, &swapchain_rtvs[index], nullptr);
	float clear_colour[4] = { 0.f, 0.f, 0.f, 0.f };
	context->ClearRenderTargetView(swapchain_rtvs[index], clear_colour);

	// (If it can't be mapped, the last pass's values are used)
	D3D11_MAPPED_SUBRESOURCE mappedConstData = {};
	if (SUCCEEDED(context->Map(constBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedConstData))) {
		ShaderConstData* constData = (ShaderConstData*)mappedConstData.pData;
		constData->offsetX = pass.offsetX;
		constData->offsetY = pass.offsetY;
		constData->scaleX = pass.scaleX;
		constData->scaleY = pass.scaleY;
		constData->decodeSrgb = pass.decodeSrgb ? 1.0f : 0.0f;
		constData->encodeSrgb = pass.encodeSrgb ? 1.0f : 0.0f;
		context->Unmap(constBuffer, 0);
	}

	context->IASetInputLayout(nullptr);
	context->PSSetShaderResources(0, 1, &view);
	context->VSSetShader(fs_vshader, nullptr, 0);
	context->PSSetShader(fs_pshader, nullptr, 0);
	context->PSSetSamplers(0, 1, &quad_sampleState);
	context->VSSetConstantBuffers(0, 1, &constBuffer);
	context->PSSetConstantBuffers(0, 1, &constBuffer);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	context->Draw(4, 0);

	if (appState) {
		// Nothing of ours left bound in our state either
		ID3D11ShaderResourceView* none = nullptr;
		context->PSSetShaderResources(0, 1, &none);
		context->OMSetRenderTargets(0, nullptr, nullptr);
		context1->SwapDeviceContextState(appState, nullptr);
		appState->Release();
		return;
	}

	context->IASetPrimitiveTopology(currTopology);
	context->IASetInputLayout(appLayout.Get());
	ID3D11Buffer* buffers[2] = { appVsBuffer.Get(), appPsBuffer.Get() };
	context->VSSetConstantBuffers(0, 1, &buffers[0]);
	context->PSSetConstantBuffers(0, 1, &buffers[1]);
	context->VSSetShader(appVs.Get(), appVsInstances, appVsInstanceCount);
	context->PSSetShader(appPs.Get(), appPsInstances, appPsInstanceCount);
	for (UINT i = 0; i < appVsInstanceCount && i < D3D11_SHADER_MAX_INTERFACES; i++)
		if (appVsInstances[i])
			appVsInstances[i]->Release();
	for (UINT i = 0; i < appPsInstanceCount && i < D3D11_SHADER_MAX_INTERFACES; i++)
		if (appPsInstances[i])
			appPsInstances[i]->Release();
	context->GSSetShader(appGs.Get(), appGsInstances, appGsInstanceCount);
	context->HSSetShader(appHs.Get(), appHsInstances, appHsInstanceCount);
	context->DSSetShader(appDs.Get(), appDsInstances, appDsInstanceCount);
	for (ID3D11ClassInstance** instances : { appGsInstances, appHsInstances, appDsInstances })
		for (UINT i = 0; i < D3D11_SHADER_MAX_INTERFACES; i++)
			if (instances[i])
				instances[i]->Release();
	ID3D11ShaderResourceView* view0 = appView.Get();
	context->PSSetShaderResources(0, 1, &view0);
	ID3D11SamplerState* sampler0 = appSampler.Get();
	context->PSSetSamplers(0, 1, &sampler0);
	context->RSSetViewports(numViewPorts, viewports.data());
	context->RSSetScissorRects(numScissors, scissors.data());
	context->RSSetState(appRasterizer.Get());
	context->OMSetBlendState(appBlend.Get(), appBlendFactor, appSampleMask);
	context->OMSetDepthStencilState(appDepthState.Get(), appStencilRef);
	context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, appTargets, appDepthView.Get());
	for (ID3D11RenderTargetView* target : appTargets)
		if (target)
			target->Release();
}

void DX11Compositor::CopyToSwapchain(const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds, std::optional<XruEye> eye, vr::EVRSubmitFlags submitFlags)
{
	auto* src = (ID3D11Texture2D*)texture->handle;

	D3D11_TEXTURE2D_DESC srcDesc;
	src->GetDesc(&srcDesc);

	// The slice shown: SubmitWithArrayIndex's, else (SteamVR takes array textures without saying what's what) the eye's
	const UINT arrayIndex = submitSlice ? *submitSlice : (srcDesc.ArraySize > 1 && eye.has_value()) ? (UINT) * eye : 0;
	if (arrayIndex >= srcDesc.ArraySize) {
		// Submit answers InvalidTexture; the eye is left out
		lastResult = XR_ERROR_VALIDATION_FAILURE;
		OC_UNSUPPORTED_VOID("A D3D11 array texture of %u slices submitted with slice %u", srcDesc.ArraySize, arrayIndex);
	}

	// The bounds' area, copied to the origin of an image of the texture's size
	TextureRegion region;
	if (!RegionFromBounds(bounds, srcDesc.Width, srcDesc.Height, region)) {
		// Submit answers InvalidTexture; the eye is left out
		lastResult = XR_ERROR_VALIDATION_FAILURE;
		OC_UNSUPPORTED_VOID("A D3D11 texture of no size (%u x %u)", srcDesc.Width, srcDesc.Height);
	}

	CheckCreateSwapChain(texture, 0, 0, false);
	if (!chain || XR_FAILED(lastResult))
		return;
	copiedArea = { (int32_t)region.Width(), (int32_t)region.Height() };

	// The shader pass converts into the swapchain's format, and flips by the bounds (invertUsingShaders: always used).
	// It needs a texture it can read: a multisampled one, or one not bound as a shader resource, is resolved or copied
	// into one first.
	const bool flip = region.flipX || region.flipY;
	const bool shaderPass = convertFormat || flip || oovr_global_configuration.InvertUsingShaders();
	if (convertFormat && !shadersUsable) {
		// Submit answers InvalidTexture; the eye is left out
		lastResult = XR_ERROR_VALIDATION_FAILURE;
		OC_UNSUPPORTED_VOID("A D3D11 texture of DXGI format %d needs the shader pass, which couldn't be set up", (int)srcDesc.Format);
	}

	// Reserve an image from the swapchain and wait until the compositor is done with it
	uint32_t currentIndex = 0;
	if (!AcquireAndWait((uint32_t)imagesHandles.size(), currentIndex))
		return;
	XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };

	// The area copied: no more than the bounds'
	D3D11_BOX sourceRegion{ region.x0, region.y0, 0, region.x1, region.y1, 1 };

	const UINT srcSubresource = D3D11CalcSubresource(0, arrayIndex, srcDesc.MipLevels);

	// Views and resolves need a typed format: a typeless texture's family member of the colour space's sRGB-ness (the
	// swapchain's own when it's copied as it is)
	const bool typeless = TypedDxgi(srcDesc.Format) != srcDesc.Format;
	const bool wantSrgb = WantsSrgb(GfxApiKind::D3D11, srcDesc.Format, texture->eColorSpace);
	const DXGI_FORMAT typedFormat = !typeless ? srcDesc.Format
	    : convertFormat                       ? (DXGI_FORMAT)FormatWithSrgb(GfxApiKind::D3D11, srcDesc.Format, wantSrgb)
	                                          : (DXGI_FORMAT)createInfo.format;

	bool useShader = shaderPass && shadersUsable && currentIndex < swapchain_rtvs.size() && swapchain_rtvs[currentIndex];
	ID3D11ShaderResourceView* view = nullptr;
	if (useShader) {
		// A one-slice array view reads the slice of an array texture, and a plain texture's only slice
		ID3D11Texture2D* from = src;
		UINT fromSlice = arrayIndex;
		if (srcDesc.SampleDesc.Count > 1 || !(srcDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE)) {
			from = ShaderSource(srcDesc, typedFormat);
			fromSlice = 0;
			if (from && srcDesc.SampleDesc.Count > 1)
				context->ResolveSubresource(from, 0, src, srcSubresource, typedFormat);
			else if (from)
				context->CopySubresourceRegion(from, 0, 0, 0, 0, src, srcSubresource, nullptr);
		}
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = typedFormat;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		srvDesc.Texture2DArray.MostDetailedMip = 0;
		srvDesc.Texture2DArray.MipLevels = 1;
		srvDesc.Texture2DArray.FirstArraySlice = fromSlice;
		srvDesc.Texture2DArray.ArraySize = 1;
		if (!from || FAILED(device->CreateShaderResourceView(from, &srvDesc, &view)) || !view) {
			OOVR_LOG_ONCE("CreateShaderResourceView failed: the texture can't go through the shader pass");
			useShader = false;
			view = nullptr;
		}
	}
	if (convertFormat && !useShader) {
		// Nothing written: the image goes back as it was, and the eye is left out (Submit answers InvalidTexture)
		xrReleaseSwapchainImage(chain, &releaseInfo);
		lastResult = XR_ERROR_VALIDATION_FAILURE;
		OC_UNSUPPORTED_VOID("A D3D11 texture of DXGI format %d couldn't be converted into swapchain format %d", (int)srcDesc.Format, (int)createInfo.format);
	}

	if (flip && !useShader)
		OOVR_LOG_ONCE("A D3D11 texture with flipped bounds couldn't go through the shader pass: shown as it is, not flipped");

	if (useShader) {
		// The area, backwards where the bounds flip it, in texture coordinates
		ShaderPass pass;
		const float w = (float)srcDesc.Width, h = (float)srcDesc.Height;
		pass.offsetX = (float)(region.flipX ? region.x1 : region.x0) / w;
		pass.offsetY = (float)(region.flipY ? region.y1 : region.y0) / h;
		pass.scaleX = (region.flipX ? -1.0f : 1.0f) * (float)region.Width() / w;
		pass.scaleY = (region.flipY ? -1.0f : 1.0f) * (float)region.Height() / h;
		pass.width = region.Width();
		pass.height = region.Height();
		// Gamma-encoded values: decoded when the view doesn't and the target encodes, encoded the other way round
		const bool viewSrgb = FormatIsSrgb(GfxApiKind::D3D11, typedFormat);
		const bool targetSrgb = FormatIsSrgb(GfxApiKind::D3D11, createInfo.format);
		pass.decodeSrgb = wantSrgb && !viewSrgb && targetSrgb;
		pass.encodeSrgb = wantSrgb && viewSrgb && !targetSrgb;
		DrawWithShader(view, currentIndex, pass);
		view->Release();
	} else {
		if (srcDesc.SampleDesc.Count > 1) {
			context->ResolveSubresource(resolvedMSAATextures[currentIndex], 0, src, srcSubresource, typedFormat);
			context->CopySubresourceRegion(imagesHandles[currentIndex].texture, 0, 0, 0, 0, resolvedMSAATextures[currentIndex], 0, &sourceRegion);
		} else {
			context->CopySubresourceRegion(imagesHandles[currentIndex].texture, 0, 0, 0, 0, src, srcSubresource, &sourceRegion);
		}
	}

	// Release the swapchain - OpenXR will use the last-released image in a swapchain
	SwapchainCallFailed(xrReleaseSwapchainImage(chain, &releaseInfo), "xrReleaseSwapchainImage");
}

void DX11Compositor::InvokeCubemap(const vr::Texture_t* textures)
{
	CheckCreateSwapChain(&textures[0], 0, 0, true);

#ifdef OC_XR_PORT
	// Not yet: SetSkyboxOverride answers RequestFailed
	OOVR_LOG_ONCE("Cube map skyboxes aren't supported yet");
	lastResult = XR_ERROR_RUNTIME_FAILURE;
	return;
	ID3D11Texture2D* tex = nullptr;
#else
	int currentIndex = 0;
	OOVR_FAILED_OVR_ABORT(ovr_GetTextureSwapChainCurrentIndex(OVSS, chain, &currentIndex));

	OOVR_FAILED_OVR_ABORT(ovr_GetTextureSwapChainBufferDX(OVSS, chain, currentIndex, IID_PPV_ARGS(&tex)));
#endif

	ID3D11Texture2D* faceSrc;

	// Front
	faceSrc = (ID3D11Texture2D*)textures[0].handle;
	context->CopySubresourceRegion(tex, 5, 0, 0, 0, faceSrc, 0, nullptr);

	// Back
	faceSrc = (ID3D11Texture2D*)textures[1].handle;
	context->CopySubresourceRegion(tex, 4, 0, 0, 0, faceSrc, 0, nullptr);

	// Left
	faceSrc = (ID3D11Texture2D*)textures[2].handle;
	context->CopySubresourceRegion(tex, 0, 0, 0, 0, faceSrc, 0, nullptr);

	// Right
	faceSrc = (ID3D11Texture2D*)textures[3].handle;
	context->CopySubresourceRegion(tex, 1, 0, 0, 0, faceSrc, 0, nullptr);

	// Top
	faceSrc = (ID3D11Texture2D*)textures[4].handle;
	context->CopySubresourceRegion(tex, 2, 0, 0, 0, faceSrc, 0, nullptr);

	// Bottom
	faceSrc = (ID3D11Texture2D*)textures[5].handle;
	context->CopySubresourceRegion(tex, 3, 0, 0, 0, faceSrc, 0, nullptr);

	tex->Release();
}

bool DX11Compositor::CopyDepth(const vr::Texture_t* depth)
{
	auto* src = (ID3D11Texture2D*)depth->handle;
	D3D11_TEXTURE2D_DESC desc;
	src->GetDesc(&desc);
	if (desc.SampleDesc.Count > 1) {
		OOVR_LOG_ONCE("A multisampled D3D11 depth texture: not sent to the runtime");
		return false;
	}
	if (!chain || createInfo.width != desc.Width || createInfo.height != desc.Height || createInfoFormat != (int64_t)desc.Format) {
		std::optional<int64_t> format = ChooseDepthSwapchainFormat(GfxApiKind::D3D11, desc.Format, RuntimeFormats());
		if (!format) {
			OOVR_LOG_ONCEF("No depth swapchain format of the runtime's for a D3D11 depth texture of DXGI format %d: depth not sent", (int)desc.Format);
			return false;
		}
		if (!MakeDepthChain(*format, desc.Width, desc.Height))
			return false;
		createInfoFormat = desc.Format;
		uint32_t count = 0;
		if (SwapchainCallFailed(xrEnumerateSwapchainImages(chain, 0, &count, nullptr), "xrEnumerateSwapchainImages (depth)"))
			return false;
		imagesHandles.assign(count, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
		if (SwapchainCallFailed(xrEnumerateSwapchainImages(chain, count, &count, (XrSwapchainImageBaseHeader*)imagesHandles.data()), "xrEnumerateSwapchainImages (depth)")) {
			imagesHandles.clear();
			xrDestroySwapchain(chain);
			chain = XR_NULL_HANDLE;
			createInfo = {};
			return false;
		}
	}
	uint32_t index = 0;
	if (!AcquireAndWait((uint32_t)imagesHandles.size(), index))
		return false;
	// A depth texture is copied whole: mip 0, slice 0
	context->CopySubresourceRegion(imagesHandles[index].texture, 0, 0, 0, 0, src, 0, nullptr);
	XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	return !SwapchainCallFailed(xrReleaseSwapchainImage(chain, &releaseInfo), "xrReleaseSwapchainImage (depth)");
}

bool DX11Compositor::ReadbackToCpu(const vr::Texture_t* tex, const vr::VRTextureBounds_t* bounds, Image& out)
{
	auto* src = tex ? (ID3D11Texture2D*)tex->handle : nullptr;
	if (!src)
		return false;
	D3D11_TEXTURE2D_DESC desc;
	src->GetDesc(&desc);
	const TexelLayout layout = TexelLayoutOfDxgi(desc.Format);
	if (layout == TexelLayout::Unknown) {
		OOVR_LOG_ONCEF("A D3D11 texture of format %d can't be read back into memory", (int)desc.Format);
		return false;
	}
	TextureRegion region;
	if (!RegionFromBounds(bounds, desc.Width, desc.Height, region))
		return false;

	// A multisampled texture is resolved first: only single-sampled areas can be copied
	ComPtr<ID3D11Texture2D> resolved;
	ID3D11Texture2D* from = src;
	const DXGI_FORMAT typed = TypedDxgi(desc.Format);
	if (desc.SampleDesc.Count > 1) {
		D3D11_TEXTURE2D_DESC rd = desc;
		rd.Format = typed;
		rd.ArraySize = 1;
		rd.MipLevels = 1;
		rd.SampleDesc = { 1, 0 };
		rd.BindFlags = 0;
		rd.CPUAccessFlags = 0;
		rd.MiscFlags = 0;
		if (FAILED(device->CreateTexture2D(&rd, nullptr, &resolved))) {
			OOVR_LOG_ONCE("Couldn't make a texture to resolve a multisampled texture into, for reading it back");
			return false;
		}
		context->ResolveSubresource(resolved.Get(), 0, src, 0, typed);
		from = resolved.Get();
	}

	D3D11_TEXTURE2D_DESC sd{};
	sd.Width = region.Width();
	sd.Height = region.Height();
	sd.MipLevels = 1;
	sd.ArraySize = 1;
	sd.Format = from == src ? desc.Format : typed;
	sd.SampleDesc = { 1, 0 };
	sd.Usage = D3D11_USAGE_STAGING;
	sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	ComPtr<ID3D11Texture2D> staging;
	if (FAILED(device->CreateTexture2D(&sd, nullptr, &staging))) {
		OOVR_LOG_ONCE("Couldn't make a staging texture to read a texture back");
		return false;
	}
	D3D11_BOX box{ region.x0, region.y0, 0, region.x1, region.y1, 1 };
	context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, from, 0, &box);
	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
		OOVR_LOG_ONCE("Couldn't map a staging texture to read a texture back");
		return false;
	}
	out = DecodeTexels(layout, mapped.pData, sd.Width, sd.Height, mapped.RowPitch, false, TexelsAreLinear(layout, tex->eColorSpace == vr::ColorSpace_Gamma));
	context->Unmap(staging.Get(), 0);
	FlipImage(out, region.flipX, region.flipY);
	return !out.Empty();
}

bool DX11Compositor::CheckChainCompatible(D3D11_TEXTURE2D_DESC& inputDesc, vr::EColorSpace colourSpace)
{
	bool usable = true;
#define FAIL(name)                             \
	do {                                       \
		usable = false;                        \
		OOVR_LOG("Resource mismatch: " #name); \
	} while (0);
#define CHECK(name, chainName)                  \
	if (inputDesc.name != createInfo.chainName) \
		FAIL(name);

	CHECK(Width, width)
	CHECK(Height, height)

	if (inputDesc.Format != createInfoFormat) {
		FAIL("Format");
	}
	if (colourSpace != createColourSpace) {
		FAIL("Colour space");
	}

	// CHECK_ADV(SampleDesc.Count, SampleCount);
	// CHECK_ADV(SampleDesc.Quality);
#undef CHECK
#undef FAIL

	return usable;
}

int DX11Compositor::MirrorView(const vr::Texture_t* texture, IUnknown* deviceOrResource, ID3D11ShaderResourceView** view)
{
	constexpr int kNone = 0, kRequestFailed = 1, kSharedTexturesNotSupported = 106;
	if (!view)
		return kRequestFailed;
	*view = nullptr;
	if (!texture || texture->eType != vr::TextureType_DirectX || !texture->handle)
		return kRequestFailed; // no D3D11 frame of the game's kept yet
	// The game's device: given, or a resource's (any D3D11 device child)
	ComPtr<ID3D11Device> device;
	if (deviceOrResource && FAILED(deviceOrResource->QueryInterface(IID_PPV_ARGS(&device)))) {
		ComPtr<ID3D11DeviceChild> child;
		if (SUCCEEDED(deviceOrResource->QueryInterface(IID_PPV_ARGS(&child))))
			child->GetDevice(&device);
	}
	if (!device)
		return kSharedTexturesNotSupported; // neither a D3D11 device nor one of its resources
	ComPtr<ID3D11Texture2D> src;
	if (FAILED(((IUnknown*)texture->handle)->QueryInterface(IID_PPV_ARGS(&src))))
		return kSharedTexturesNotSupported;
	ComPtr<ID3D11Device> srcDevice;
	src->GetDevice(&srcDevice);
	if (srcDevice != device)
		return kSharedTexturesNotSupported;

	D3D11_TEXTURE2D_DESC desc;
	src->GetDesc(&desc);
	bool srgb = WantsSrgb(GfxApiKind::D3D11, desc.Format, texture->eColorSpace);
	DXGI_FORMAT typed = (DXGI_FORMAT)FormatWithSrgb(GfxApiKind::D3D11, desc.Format, srgb);
	D3D11_TEXTURE2D_DESC copyDesc = desc;
	copyDesc.Format = typed;
	copyDesc.MipLevels = 1;
	copyDesc.ArraySize = 1;
	copyDesc.SampleDesc = { 1, 0 };
	copyDesc.Usage = D3D11_USAGE_DEFAULT;
	copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	copyDesc.CPUAccessFlags = 0;
	copyDesc.MiscFlags = 0;
	ID3D11Texture2D* copy = nullptr;
	if (FAILED(device->CreateTexture2D(&copyDesc, nullptr, &copy)))
		return kRequestFailed;
	ID3D11DeviceContext* ctx = nullptr;
	device->GetImmediateContext(&ctx);
	if (desc.SampleDesc.Count > 1)
		ctx->ResolveSubresource(copy, 0, src.Get(), 0, typed);
	else
		ctx->CopySubresourceRegion(copy, 0, 0, 0, 0, src.Get(), 0, nullptr);
	ctx->Release();
	HRESULT hr = device->CreateShaderResourceView(copy, nullptr, view);
	copy->Release(); // the view keeps it
	return SUCCEEDED(hr) ? kNone : kRequestFailed;
}

#endif
