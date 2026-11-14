// The frame cases, gfx-<api>-<what>: a game on one graphics API (gfx/GfxApi.h) submits the test pattern, and the
// stand-in runtime's dump shows what OpenLamina made of it. Each records "frame" with "dump_join":
// "last_projection", for which run.py joins the dump's last projection layer (per view: samples, rect, format,
// invalid) as the record's "dump"; a case whose API can't be made here records {"skipped": why}.

#include "conformance.h"

#include "gfx/GfxApi.h"

#include "IVRCompositor_029.h"
#include "IVROverlay_028.h"

#include <cstring>

using namespace conformance;

std::unique_ptr<GfxApi> GfxApi::Create(GfxKind kind, std::string* whyNot)
{
#ifdef _WIN32
	switch (kind) {
	case GfxKind::D3D11:
		return MakeD3D11Gfx(whyNot);
	case GfxKind::D3D12:
		return MakeD3D12Gfx(whyNot);
	case GfxKind::Vulkan:
		return MakeVkGfx(whyNot);
	case GfxKind::OpenGL:
		return MakeGLGfx(whyNot);
	case GfxKind::D3D10:
		return MakeD3D10Gfx(whyNot);
	}
	*whyNot = "unknown graphics API";
	return nullptr;
#else
	(void)kind;
	*whyNot = "the frame cases' graphics are Windows only";
	return nullptr;
#endif
}

namespace {

// Formats in each API's own enum: RGBA8 sRGB
constexpr int64_t kDxgiRGBA8Srgb = 29; // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
constexpr int64_t kVkRGBA8Srgb = 43; // VK_FORMAT_R8G8B8A8_SRGB
constexpr int64_t kGlSRGB8Alpha8 = 0x8C43; // GL_SRGB8_ALPHA8

// The frames each case submits: enough for OpenLamina to move its session onto the game's device
constexpr int kFrames = 10;

Json::Value Skipped(const std::string& why)
{
	Json::Value r;
	r["skipped"] = why;
	return r;
}

// 1024x1024 per eye, full bounds, kFrames frames
void Basic(CaseContext& c, GfxKind kind, int64_t format)
{
	std::string why;
	std::unique_ptr<GfxApi> gfx = GfxApi::Create(kind, &why);
	if (!gfx) {
		c.Record("frame", Skipped(why));
		return;
	}
	GfxTexture left = gfx->Pattern(1024, 1024, format, 0);
	GfxTexture right = gfx->Pattern(1024, 1024, format, 1);
	gfx->Flush();
	if (!left.Made() || !right.Made()) {
		Json::Value r;
		r["error"] = "the pattern textures couldn't be made";
		c.Record("frame", r);
		return;
	}
	if (c.core->Init(vr::VRApplication_Scene, nullptr) != vr::VRInitError_None) {
		c.Record("Init failed");
		return;
	}
	vr::EVRInitError le = vr::VRInitError_None;
	auto* comp = (vr::IVRCompositor_029::IVRCompositor*)c.core->GetGenericInterface("IVRCompositor_029", &le);
	if (!comp) {
		Json::Value r;
		r["lookup_error"] = (int)le;
		c.Record("no compositor", r);
		c.core->Cleanup();
		return;
	}
	vr::VRTextureBounds_t bounds{ 0, 0, 1, 1 };
	Json::Value r;
	for (int frame = 1; frame <= kFrames; frame++) {
		vr::TrackedDevicePose_t poses[16];
		r["wait"] = (int)comp->WaitGetPoses(poses, 16, nullptr, 0);
		r["left"] = (int)comp->Submit(vr::Eye_Left, left.Texture(), &bounds, vr::Submit_Default);
		r["right"] = (int)comp->Submit(vr::Eye_Right, right.Texture(), &bounds, vr::Submit_Default);
	}
	r["frames"] = kFrames;
	r["dump_join"] = "last_projection";
	c.Record("frame", r);
	// OpenLamina first, then the textures and the device (OpenVR's rule for every API)
	c.core->Cleanup();
	gfx->Flush();
}

// The test pattern of eye 0, w x h, as RGBA8 bytes, rows top to bottom
std::vector<uint8_t> PatternBytes(uint32_t w, uint32_t h)
{
	std::vector<uint8_t> bytes((size_t)w * h * 4);
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++) {
			uint32_t c = gfx::PatternColour(x, y, w, h, 0);
			uint8_t* p = &bytes[((size_t)y * w + x) * 4];
			p[0] = (uint8_t)(c >> 24), p[1] = (uint8_t)(c >> 16), p[2] = (uint8_t)(c >> 8), p[3] = (uint8_t)c;
		}
	return bytes;
}

// GetOverlayImageData of the overlay: first with a buffer one byte short (ArrayTooSmall, with the size), then a big
// enough one, whose bytes are compared with expected
void RecordImageData(CaseContext& c, vr::IVROverlay_028::IVROverlay* ov, vr::VROverlayHandle_t h, const std::vector<uint8_t>& expected, Json::Value& r)
{
	std::vector<uint8_t> buffer(expected.size() + 16, 0xEE);
	uint32_t w = 0, hh = 0;
	r["short"] = (int)ov->GetOverlayImageData(h, buffer.data(), (uint32_t)expected.size() - 1, &w, &hh);
	r["short_size"] = Json::Value(Json::arrayValue);
	r["short_size"].append(w);
	r["short_size"].append(hh);
	w = hh = 0;
	r["get"] = (int)ov->GetOverlayImageData(h, buffer.data(), (uint32_t)buffer.size(), &w, &hh);
	r["size"] = Json::Value(Json::arrayValue);
	r["size"].append(w);
	r["size"].append(hh);
	r["same"] = std::memcmp(buffer.data(), expected.data(), expected.size()) == 0;
	(void)c;
}

// A scene app on the API shows an overlay set from memory (SetOverlayRaw, 64x64 RGBA, set before the first frame
// so the session moves onto the game's device after it), and reads it back
void OverlayRaw(CaseContext& c, GfxKind kind, int64_t format)
{
	std::string why;
	std::unique_ptr<GfxApi> gfx = GfxApi::Create(kind, &why);
	if (!gfx) {
		c.Record("overlay", Skipped(why));
		return;
	}
	GfxTexture left = gfx->Pattern(256, 256, format, 0);
	GfxTexture right = gfx->Pattern(256, 256, format, 1);
	gfx->Flush();
	if (!left.Made() || !right.Made()) {
		Json::Value r;
		r["error"] = "the pattern textures couldn't be made";
		c.Record("overlay", r);
		return;
	}
	if (c.core->Init(vr::VRApplication_Scene, nullptr) != vr::VRInitError_None) {
		c.Record("Init failed");
		return;
	}
	vr::EVRInitError le = vr::VRInitError_None;
	auto* comp = (vr::IVRCompositor_029::IVRCompositor*)c.core->GetGenericInterface("IVRCompositor_029", &le);
	auto* ov = (vr::IVROverlay_028::IVROverlay*)c.core->GetGenericInterface("IVROverlay_028", &le);
	if (!comp || !ov) {
		Json::Value r;
		r["lookup_error"] = (int)le;
		c.Record("no compositor or overlay", r);
		c.core->Cleanup();
		return;
	}
	Json::Value r;
	vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid;
	r["create"] = (int)ov->CreateOverlay("oc.test.raw", "Raw", &h);
	std::vector<uint8_t> pattern = PatternBytes(64, 64);
	r["set"] = (int)ov->SetOverlayRaw(h, pattern.data(), 64, 64, 4);
	r["show"] = (int)ov->ShowOverlay(h);
	vr::VRTextureBounds_t bounds{ 0, 0, 1, 1 };
	for (int frame = 1; frame <= kFrames; frame++) {
		vr::TrackedDevicePose_t poses[16];
		comp->WaitGetPoses(poses, 16, nullptr, 0);
		r["left"] = (int)comp->Submit(vr::Eye_Left, left.Texture(), &bounds, vr::Submit_Default);
		r["right"] = (int)comp->Submit(vr::Eye_Right, right.Texture(), &bounds, vr::Submit_Default);
	}
	RecordImageData(c, ov, h, pattern, r);
	r["dump_join"] = "last_layer:quad";
	c.Record("overlay", r);
	ov->DestroyOverlay(h);
	c.core->Cleanup();
	gfx->Flush();
}

// A texture overlay's GetOverlayImageData: the texture read back (64x64 RGBA8 sRGB)
void OverlayTextureReadback(CaseContext& c, GfxKind kind, int64_t format)
{
	std::string why;
	std::unique_ptr<GfxApi> gfx = GfxApi::Create(kind, &why);
	if (!gfx) {
		c.Record("readback", Skipped(why));
		return;
	}
	GfxTexture tex = gfx->Pattern(64, 64, format, 0);
	gfx->Flush();
	if (!tex.Made()) {
		Json::Value r;
		r["error"] = "the pattern texture couldn't be made";
		c.Record("readback", r);
		return;
	}
	if (c.core->Init(vr::VRApplication_Scene, nullptr) != vr::VRInitError_None) {
		c.Record("Init failed");
		return;
	}
	vr::EVRInitError le = vr::VRInitError_None;
	auto* ov = (vr::IVROverlay_028::IVROverlay*)c.core->GetGenericInterface("IVROverlay_028", &le);
	if (!ov) {
		Json::Value r;
		r["lookup_error"] = (int)le;
		c.Record("no overlay", r);
		c.core->Cleanup();
		return;
	}
	Json::Value r;
	vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid;
	r["create"] = (int)ov->CreateOverlay("oc.test.texture", "Texture", &h);
	r["set"] = (int)ov->SetOverlayTexture(h, tex.Texture());
	RecordImageData(c, ov, h, PatternBytes(64, 64), r);
	c.Record("readback", r);
	ov->DestroyOverlay(h);
	c.core->Cleanup();
	gfx->Flush();
}

} // namespace

const std::map<std::string, CaseFn>& conformance::GfxCases()
{
	static const std::map<std::string, CaseFn> cases = {
		{ "gfx-d3d11-basic", [](CaseContext& c) { Basic(c, GfxKind::D3D11, kDxgiRGBA8Srgb); } },
		{ "gfx-d3d12-basic", [](CaseContext& c) { Basic(c, GfxKind::D3D12, kDxgiRGBA8Srgb); } },
		{ "gfx-vk-basic", [](CaseContext& c) { Basic(c, GfxKind::Vulkan, kVkRGBA8Srgb); } },
		{ "gfx-gl-basic", [](CaseContext& c) { Basic(c, GfxKind::OpenGL, kGlSRGB8Alpha8); } },
		{ "overlay-raw-d3d11", [](CaseContext& c) { OverlayRaw(c, GfxKind::D3D11, kDxgiRGBA8Srgb); } },
		{ "overlay-raw-d3d12", [](CaseContext& c) { OverlayRaw(c, GfxKind::D3D12, kDxgiRGBA8Srgb); } },
		{ "overlay-raw-vk", [](CaseContext& c) { OverlayRaw(c, GfxKind::Vulkan, kVkRGBA8Srgb); } },
		{ "overlay-raw-gl", [](CaseContext& c) { OverlayRaw(c, GfxKind::OpenGL, kGlSRGB8Alpha8); } },
		{ "overlay-texture-readback-d3d11", [](CaseContext& c) { OverlayTextureReadback(c, GfxKind::D3D11, kDxgiRGBA8Srgb); } },
		{ "overlay-texture-readback-d3d12", [](CaseContext& c) { OverlayTextureReadback(c, GfxKind::D3D12, kDxgiRGBA8Srgb); } },
		{ "overlay-texture-readback-vk", [](CaseContext& c) { OverlayTextureReadback(c, GfxKind::Vulkan, kVkRGBA8Srgb); } },
		{ "overlay-texture-readback-gl", [](CaseContext& c) { OverlayTextureReadback(c, GfxKind::OpenGL, kGlSRGB8Alpha8); } },
	};
	return cases;
}
