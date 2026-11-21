// The frame cases, gfx-<api>-<what>: a game on one graphics API (gfx/GfxApi.h) submits the test pattern, and the
// stand-in runtime's dump shows what OpenLamina made of it. Each records "frame" with "dump_join":
// "last_projection", for which run.py joins the dump's last projection layer (per view: samples, rect, format,
// invalid) as the record's "dump"; a case whose API can't be made here records {"skipped": why}.

#include "conformance.h"

#include "gfx/GfxApi.h"
#include "../PngWriter.h"

#include "IVRCompositor_029.h"
#include "IVROverlay_028.h"
#include "IVRSystem_022.h"

#ifdef _WIN32
#include <d3d11.h>
#include <d3d12.h>
#endif

#include <algorithm>
#include <fstream>
#include <filesystem>
#include <cstdio>
#include <thread>
#include <map>
#include <chrono>
#include <cstring>
#include <cstdlib>
#ifndef _WIN32
#include <unistd.h>
#endif

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
	switch (kind) {
	case GfxKind::Vulkan:
		return MakeVkGfx(whyNot);
	case GfxKind::OpenGL:
		return MakeGLGfx(whyNot);
	default:
		*whyNot = "Direct3D is Windows only";
		return nullptr;
	}
#endif
}

std::unique_ptr<GfxApi> GfxApi::CreateOnWarp(GfxKind kind, std::string* whyNot)
{
#ifdef _WIN32
	if (kind == GfxKind::D3D12)
		return MakeD3D12Gfx(whyNot, true);
#endif
	return Create(kind, whyNot);
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
// The file OpenLamina's CompositorDumpImages writes an eye's image to (BaseCompositor::DumpImageName)
std::string BaseCompositorDumpName(vr::EVREye eye)
{
#ifdef _WIN32
	const unsigned long pid = GetCurrentProcessId();
#else
	const unsigned long pid = (unsigned long)getpid();
#endif
	return "compositor-dump." + std::to_string(pid) + (eye == vr::Eye_Left ? ".left.png" : ".right.png");
}

void Basic(CaseContext& c, GfxKind kind, int64_t format, bool dump = false)
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
		// CompositorDumpImages: the next frame's eyes, as PNGs in the log folder
		if (dump && frame == 2)
			comp->CompositorDumpImages();
	}
	r["frames"] = kFrames;
	r["dump_join"] = "last_projection";
	c.Record("frame", r);
	// OpenLamina first, then the textures and the device (OpenVR's rule for every API)
	c.core->Cleanup();
	gfx->Flush();
	if (dump) {
		Json::Value d;
		const char* logs = std::getenv("OC_LOG_DIR");
		const std::filesystem::path folder = logs ? logs : ".";
		for (vr::EVREye eye : { vr::Eye_Left, vr::Eye_Right }) {
			std::error_code ec;
			const std::filesystem::path png = folder / BaseCompositorDumpName(eye);
			const auto size = std::filesystem::file_size(png, ec);
			d[eye == vr::Eye_Left ? "left_png" : "right_png"] = !ec && size > 1000;
		}
		c.Record("dump images", d);
	}
}

// A game of the API that submits a few frames, then calls WaitGetPoses without Submit: those frames are ended for it
// inside the call (its last frame again), on the thread it renders on (and, for OpenGL, with its context current)
void WaitWithoutSubmit(CaseContext& c, GfxKind kind, int64_t format)
{
	std::string why;
	std::unique_ptr<GfxApi> gfx = GfxApi::Create(kind, &why);
	if (!gfx) {
		c.Record("frames", Skipped(why));
		return;
	}
	GfxTexture eye = gfx->Pattern(256, 256, format, 0);
	gfx->Flush();
	if (!eye.Made() || c.core->Init(vr::VRApplication_Scene, nullptr) != vr::VRInitError_None) {
		c.Record("Init failed");
		return;
	}
	vr::EVRInitError le = vr::VRInitError_None;
	auto* comp = (vr::IVRCompositor_029::IVRCompositor*)c.core->GetGenericInterface("IVRCompositor_029", &le);
	if (!comp) {
		c.Record("no compositor");
		c.core->Cleanup();
		return;
	}
	vr::VRTextureBounds_t bounds{ 0, 0, 1, 1 };
	vr::TrackedDevicePose_t poses[16];
	for (int frame = 1; frame <= 5; frame++) {
		comp->WaitGetPoses(poses, 16, nullptr, 0);
		comp->Submit(vr::Eye_Left, eye.Texture(), &bounds, vr::Submit_Default);
		comp->Submit(vr::Eye_Right, eye.Texture(), &bounds, vr::Submit_Default);
	}
	for (int frame = 1; frame <= 20; frame++)
		comp->WaitGetPoses(poses, 16, nullptr, 0);
	c.core->Cleanup();
	gfx->Flush();
	Json::Value dump = ReadStandInDump();
	Json::Value r;
	r["frames_ended_at_least_22"] = dump["frames_ended"].asInt64() >= 22;
	r["frame_order_errors"] = dump["frame_order_errors"];
	c.Record("frames", r);
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

// A scene app on one graphics API: its device, OpenLamina's compositor, and frames of its textures
struct Scene {
	CaseContext& c;
	std::unique_ptr<GfxApi> gfx;
	vr::IVRCompositor_029::IVRCompositor* comp = nullptr;
	bool started = false;

	explicit Scene(CaseContext& ctx)
	    : c(ctx)
	{
	}

	// False, with label recorded as skipped (no such device here) or failed, if it can't start
	bool Start(GfxKind kind, const std::string& label)
	{
		std::string why;
		gfx = GfxApi::Create(kind, &why);
		if (!gfx) {
			c.Record(label, Skipped(why));
			return false;
		}
		if (c.core->Init(vr::VRApplication_Scene, nullptr) != vr::VRInitError_None) {
			c.Record("Init failed");
			gfx.reset();
			return false;
		}
		started = true;
		vr::EVRInitError le = vr::VRInitError_None;
		comp = (vr::IVRCompositor_029::IVRCompositor*)c.core->GetGenericInterface("IVRCompositor_029", &le);
		if (!comp) {
			Json::Value r;
			r["lookup_error"] = (int)le;
			c.Record("no compositor", r);
			return false;
		}
		return true;
	}

	// n frames of left and right with their bounds, through SubmitWithArrayIndex when slice >= 0; the last frame's
	// answers in r["left"] and r["right"]
	void Frames(int n, GfxTexture& left, GfxTexture& right, const vr::VRTextureBounds_t& lb, const vr::VRTextureBounds_t& rb, Json::Value& r,
	    vr::EVRSubmitFlags flags = vr::Submit_Default, int slice = -1)
	{
		for (int frame = 1; frame <= n; frame++) {
			vr::TrackedDevicePose_t poses[16];
			comp->WaitGetPoses(poses, 16, nullptr, 0);
			if (slice >= 0) {
				r["left"] = (int)comp->SubmitWithArrayIndex(vr::Eye_Left, left.Texture(), (uint32_t)slice, &lb, flags);
				r["right"] = (int)comp->SubmitWithArrayIndex(vr::Eye_Right, right.Texture(), (uint32_t)slice, &rb, flags);
			} else {
				r["left"] = (int)comp->Submit(vr::Eye_Left, left.Texture(), &lb, flags);
				r["right"] = (int)comp->Submit(vr::Eye_Right, right.Texture(), &rb, flags);
			}
		}
	}

	~Scene()
	{
		// OpenLamina first, then the textures and the device (OpenVR's rule for every API)
		if (started)
			c.core->Cleanup();
		if (gfx)
			gfx->Flush();
	}
};

constexpr vr::VRTextureBounds_t kFull{ 0, 0, 1, 1 };

std::string RectJoin(uint32_t w, uint32_t h)
{
	return "projection_rect:" + std::to_string(w) + "x" + std::to_string(h);
}

// One texture per eye of each format, a size of its own each (the dump's projection layer is found by its rect), under
// a script whose runtime offers RGBA8 sRGB only: each is converted into it. samples > 1: multisampled textures
// (resolved, then converted), which have the quadrants alone.
void Formats(CaseContext& c, GfxKind kind, const std::vector<std::pair<std::string, int64_t>>& formats, uint32_t samples = 1)
{
	Scene s(c);
	if (!s.Start(kind, "format " + formats[0].first))
		return;
	uint32_t size = 512;
	bool first = true;
	for (const auto& [name, format] : formats) {
		GfxTexture left = s.gfx->Pattern(size, size, format, 0, samples);
		GfxTexture right = s.gfx->Pattern(size, size, format, 1, samples);
		s.gfx->Flush();
		Json::Value r;
		if (!left.Made() || !right.Made()) {
			r["error"] = "the pattern textures couldn't be made";
		} else {
			s.Frames(first ? kFrames : 4, left, right, kFull, kFull, r);
			r["dump_join"] = RectJoin(size, size);
		}
		c.Record("format " + name, r);
		first = false;
		size -= 64;
	}
}

// Textures games send that are out of the ordinary, D3D11: each its own size (found by its rect in the dump)
void D3D11Odd(CaseContext& c)
{
	Scene s(c);
	if (!s.Start(GfxKind::D3D11, "1x1"))
		return;
	struct Odd {
		const char* name;
		uint32_t w, h;
		int64_t format;
		uint32_t samples, layers;
		int slice;
		vr::EColorSpace colourSpace;
	};
	const Odd odds[] = {
		{ "1x1", 1, 1, kDxgiRGBA8Srgb, 1, 1, -1, vr::ColorSpace_Auto },
		{ "4097x2049", 4097, 2049, kDxgiRGBA8Srgb, 1, 1, -1, vr::ColorSpace_Auto },
		{ "msaa typeless", 600, 600, 27 /* DXGI_FORMAT_R8G8B8A8_TYPELESS */, 4, 1, -1, vr::ColorSpace_Auto },
		{ "array slice 1", 320, 320, kDxgiRGBA8Srgb, 1, 2, 1, vr::ColorSpace_Auto },
		{ "srgb under linear", 200, 200, kDxgiRGBA8Srgb, 1, 1, -1, vr::ColorSpace_Linear },
	};
	bool first = true;
	for (const Odd& o : odds) {
		GfxTexture left = s.gfx->Pattern(o.w, o.h, o.format, 0, o.samples, o.layers);
		GfxTexture right = s.gfx->Pattern(o.w, o.h, o.format, 1, o.samples, o.layers);
		s.gfx->Flush();
		left.tex.eColorSpace = right.tex.eColorSpace = o.colourSpace;
		Json::Value r;
		if (!left.Made() || !right.Made()) {
			r["error"] = "the pattern textures couldn't be made";
		} else {
			s.Frames(first ? kFrames : 4, left, right, kFull, kFull, r, vr::Submit_Default, o.slice);
			r["dump_join"] = RectJoin(o.w, o.h);
		}
		c.Record(o.name, r);
		first = false;
	}

	// Bounds left all zero (a mod that never fills them in): the whole texture is shown
	{
		const vr::VRTextureBounds_t zero{ 0, 0, 0, 0 };
		GfxTexture left = s.gfx->Pattern(240, 240, kDxgiRGBA8Srgb, 0);
		GfxTexture right = s.gfx->Pattern(240, 240, kDxgiRGBA8Srgb, 1);
		s.gfx->Flush();
		Json::Value r;
		if (!left.Made() || !right.Made()) {
			r["error"] = "the pattern textures couldn't be made";
		} else {
			s.Frames(4, left, right, zero, zero, r);
			r["dump_join"] = RectJoin(240, 240);
		}
		c.Record("zero bounds", r);
	}
}

// Bounds: the whole texture, side-by-side halves of one texture, the whole flipped (vMin > vMax), and flipped halves.
// Each has its own size (found by its rect in the dump): 768 x 768 per eye, halves of 2048 x 1024, 640 x 640, halves
// of 1200 x 600. One texture of eye 0's pattern serves both eyes' halves. --case-arg frames=<n> shows each n frames
// (4 by default, at most 900), long enough to see on a real runtime.
void Bounds(CaseContext& c, GfxKind kind, int64_t format)
{
	const int each = std::min(std::max(c.IntArg("frames", 4), 1), 900);
	Scene s(c);
	if (!s.Start(kind, "full"))
		return;
	struct Config {
		const char* name;
		uint32_t w, h;
		bool sideBySide;
		vr::VRTextureBounds_t left, right;
		uint32_t rectW, rectH;
	};
	const Config configs[] = {
		{ "full", 768, 768, false, { 0, 0, 1, 1 }, { 0, 0, 1, 1 }, 768, 768 },
		{ "halves", 2048, 1024, true, { 0, 0, 0.5f, 1 }, { 0.5f, 0, 1, 1 }, 1024, 1024 },
		{ "flipped", 640, 640, false, { 0, 1, 1, 0 }, { 0, 1, 1, 0 }, 640, 640 },
		{ "flipped halves", 1200, 600, true, { 0, 1, 0.5f, 0 }, { 0.5f, 1, 1, 0 }, 600, 600 },
		// One above the other, the left eye at v from 0 to a half: the top half as shown, or for OpenGL, whose v runs
		// up from the bottom row, the bottom half
		{ "stacked", 896, 1792, true, { 0, 0, 1, 0.5f }, { 0, 0.5f, 1, 1 }, 896, 896 },
	};
	bool first = true;
	for (const Config& cfg : configs) {
		GfxTexture left = s.gfx->Pattern(cfg.w, cfg.h, format, 0);
		GfxTexture right = cfg.sideBySide ? left : s.gfx->Pattern(cfg.w, cfg.h, format, 1);
		s.gfx->Flush();
		Json::Value r;
		if (!left.Made() || !right.Made()) {
			r["error"] = "the pattern textures couldn't be made";
		} else {
			s.Frames(first ? std::max(kFrames, each) : each, left, right, cfg.left, cfg.right, r);
			r["dump_join"] = RectJoin(cfg.rectW, cfg.rectH);
		}
		c.Record(cfg.name, r);
		first = false;
	}
}

// Vulkan textures out of the ordinary, under a script whose runtime has single-sampled swapchains only (so a multisampled
// texture is resolved): 4x MSAA RGBA8 sRGB of 600 x 600 with full bounds, the same of 640 x 640 with flipped bounds
// (resolved, then flipped by a blit), and 512 x 512 textures filled on, and submitted with, a second queue of the first
// one's family (skipped where the family has one queue), done there before they're submitted.
// Multisampled patterns have the quadrants alone. Each found in the dump by its size.
void VkOdd(CaseContext& c)
{
	Scene s(c);
	if (!s.Start(GfxKind::Vulkan, "msaa"))
		return;
	struct Odd {
		const char* name;
		uint32_t size, samples;
		vr::VRTextureBounds_t bounds;
		bool otherQueue;
	};
	const Odd odds[] = {
		{ "msaa", 600, 4, kFull, false },
		{ "msaa flipped", 640, 4, { 0, 1, 1, 0 }, false },
		{ "other queue", 512, 1, kFull, true },
	};
	bool first = true;
	for (const Odd& o : odds) {
		Json::Value r;
		if (o.otherQueue && !s.gfx->UseOtherQueue()) {
			c.Record(o.name, Skipped("the queue family has one queue"));
			continue;
		}
		GfxTexture left = s.gfx->Pattern(o.size, o.size, kVkRGBA8Srgb, 0, o.samples);
		GfxTexture right = s.gfx->Pattern(o.size, o.size, kVkRGBA8Srgb, 1, o.samples);
		s.gfx->Flush();
		if (!left.Made() || !right.Made()) {
			r["error"] = "the pattern textures couldn't be made";
		} else {
			s.Frames(first ? kFrames : 4, left, right, o.bounds, o.bounds, r);
			r["dump_join"] = RectJoin(o.size, o.size);
		}
		c.Record(o.name, r);
		first = false;
	}
}

// A Vulkan game whose textures change size from frame to frame (dynamic resolution): 800, 640, 720 and then 960 pixels a
// side, each for 4 frames. Each size's view shows its whole texture; the swapchain under it (image) is made again
// only when the texture grows past it: 800 x 800 for the first three, 960 x 960 for the last.
void VkDynamicResolution(CaseContext& c)
{
	Scene s(c);
	if (!s.Start(GfxKind::Vulkan, "800"))
		return;
	bool first = true;
	for (uint32_t size : { 800u, 640u, 720u, 960u }) {
		GfxTexture left = s.gfx->Pattern(size, size, kVkRGBA8Srgb, 0);
		GfxTexture right = s.gfx->Pattern(size, size, kVkRGBA8Srgb, 1);
		s.gfx->Flush();
		Json::Value r;
		if (!left.Made() || !right.Made()) {
			r["error"] = "the pattern textures couldn't be made";
		} else {
			s.Frames(first ? kFrames : 4, left, right, kFull, kFull, r);
			r["dump_join"] = RectJoin(size, size);
		}
		c.Record(std::to_string(size), r);
		first = false;
	}
}

// A game that makes its device again mid-run (as after a device removal, or a GPU switch in its settings): 512 x 512
// textures of its first device for 10 frames, then 576 x 576 ones of a new device of the same API for 10 frames.
// D3D11 and D3D12: the session follows the textures onto the new device; under a script whose runtime refuses that
// session (the third xrCreateSession), the game's frames are left out. Vulkan: the new device's are refused. The game
// isn't told to quit ("quit": a VREvent_Quit came).
void NewDevice(CaseContext& c, GfxKind kind, int64_t format)
{
	Scene s(c);
	if (!s.Start(kind, "first device"))
		return;
	vr::EVRInitError le = vr::VRInitError_None;
	auto* sys = (vr::IVRSystem_022::IVRSystem*)c.core->GetGenericInterface("IVRSystem_022", &le);
	bool quit = false;
	auto frames = [&](GfxTexture& left, GfxTexture& right, Json::Value& r) {
		for (int frame = 1; frame <= kFrames; frame++) {
			s.Frames(1, left, right, kFull, kFull, r);
			vr::VREvent_t ev{};
			while (sys && sys->PollNextEvent(&ev, sizeof(ev)))
				quit = quit || ev.eventType == vr::VREvent_Quit;
		}
	};
	{
		GfxTexture left = s.gfx->Pattern(512, 512, format, 0);
		GfxTexture right = s.gfx->Pattern(512, 512, format, 1);
		s.gfx->Flush();
		Json::Value r;
		if (!left.Made() || !right.Made()) {
			r["error"] = "the pattern textures couldn't be made";
		} else {
			frames(left, right, r);
			r["dump_join"] = RectJoin(512, 512);
		}
		c.Record("first device", r);
	}
	std::string why;
	std::unique_ptr<GfxApi> second = GfxApi::CreateOnWarp(kind, &why);
	if (!second) {
		c.Record("new device", Skipped(why));
		return;
	}
	GfxTexture left = second->Pattern(576, 576, format, 0);
	GfxTexture right = second->Pattern(576, 576, format, 1);
	second->Flush();
#ifdef _WIN32
	if (kind == GfxKind::D3D12 && left.Made()) {
		// One D3D12 device per adapter: with WARP the only adapter, the second device is the first
		GfxTexture probe = s.gfx->Pattern(8, 8, format, 0);
		ID3D12Device *a = nullptr, *b = nullptr;
		if (probe.Made())
			probe.d3d12.m_pResource->GetDevice(IID_PPV_ARGS(&a));
		left.d3d12.m_pResource->GetDevice(IID_PPV_ARGS(&b));
		const bool same = a == b;
		if (a)
			a->Release();
		if (b)
			b->Release();
		if (same) {
			c.Record("new device", Skipped("one D3D12 adapter: the second device is the first"));
			c.core->Cleanup();
			s.started = false;
			return;
		}
	}
#endif
	Json::Value r;
	if (!left.Made() || !right.Made()) {
		r["error"] = "the pattern textures couldn't be made";
	} else {
		frames(left, right, r);
		r["dump_join"] = RectJoin(576, 576);
	}
	r["quit"] = quit;
	c.Record("new device", r);
	// OpenLamina first, then the textures and the devices
	c.core->Cleanup();
	s.started = false;
	second->Flush();
}

// A D3D11 game whose eyes are on two devices (the left eye on its first one, the right on a second), 10 frames: the
// session follows the eye on the other device a few times (ending on the right eye's), and then the left is left out. No frame names a swapchain of a session
// that's gone ("no_invalid_layers"), and the game isn't told to quit.
void EyesOnTwoDevices(CaseContext& c)
{
	Scene s(c);
	if (!s.Start(GfxKind::D3D11, "eyes on two devices"))
		return;
	vr::EVRInitError le = vr::VRInitError_None;
	auto* sys = (vr::IVRSystem_022::IVRSystem*)c.core->GetGenericInterface("IVRSystem_022", &le);
	std::string why;
	std::unique_ptr<GfxApi> second = GfxApi::Create(GfxKind::D3D11, &why);
	if (!second) {
		c.Record("eyes on two devices", Skipped(why));
		return;
	}
	GfxTexture left = s.gfx->Pattern(512, 512, kDxgiRGBA8Srgb, 0);
	GfxTexture right = second->Pattern(512, 512, kDxgiRGBA8Srgb, 1);
	s.gfx->Flush();
	second->Flush();
	Json::Value r;
	bool quit = false;
	for (int frame = 1; frame <= kFrames; frame++) {
		s.Frames(1, left, right, kFull, kFull, r);
		vr::VREvent_t ev{};
		while (sys && sys->PollNextEvent(&ev, sizeof(ev)))
			quit = quit || ev.eventType == vr::VREvent_Quit;
	}
	r["quit"] = quit;
	c.core->Cleanup();
	s.started = false;
	second->Flush();
	Json::Value dump = ReadStandInDump();
	if (!dump.isNull()) {
		bool clean = true;
		for (const Json::Value& layer : dump["layers"]) {
			clean = clean && layer["invalid"].empty();
			for (const Json::Value& view : layer["views"])
				clean = clean && view["invalid"].empty();
		}
		r["no_invalid_layers"] = clean;
	}
	c.Record("eyes on two devices", r);
}

// The pose a frame was rendered with and its depth: each eye's texture goes with a head pose 0.5 m to the left of the
// one WaitGetPoses gave, and a depth texture of the colour's size of 0.25 everywhere from a projection with near 0.1 and
// far 100 (Submit_TextureWithPose | Submit_TextureWithDepth). "pose-depth": 512 x 512 per eye, full bounds; "halves":
// side-by-side halves of one 1536 x 768 texture and its depth (the depth's rect is the colour's half); "flipped":
// 640 x 640 per eye with flipped bounds (no depth goes with it). Each has its own size, found by its rect in the dump.
// The first records the located head's position as "head".
void PoseDepth(CaseContext& c, GfxKind kind, int64_t format)
{
	Scene s(c);
	if (!s.Start(kind, "pose-depth"))
		return;
	// OpenVR's projection (D3D convention) for near 0.1 and far 100
	const float n = 0.1f, f = 100.0f;
	vr::HmdMatrix44_t projection{};
	projection.m[0][0] = 1;
	projection.m[1][1] = 1;
	projection.m[2][2] = f / (n - f);
	projection.m[2][3] = n * f / (n - f);
	projection.m[3][2] = -1;
	struct Config {
		const char* name;
		uint32_t w, h;
		bool sideBySide;
		vr::VRTextureBounds_t left, right;
		uint32_t rectW, rectH;
		bool vkImageOnly = false; // Vulkan: the depth's struct names only its image, not the device it's on
	};
	const Config configs[] = {
		{ "pose-depth", 512, 512, false, { 0, 0, 1, 1 }, { 0, 0, 1, 1 }, 512, 512 },
		{ "halves", 1536, 768, true, { 0, 0, 0.5f, 1 }, { 0.5f, 0, 1, 1 }, 768, 768 },
		{ "flipped", 640, 640, false, { 0, 1, 1, 0 }, { 0, 1, 1, 0 }, 640, 640 },
		{ "depth image only", 576, 576, false, { 0, 0, 1, 1 }, { 0, 0, 1, 1 }, 576, 576, true },
	};
	bool first = true;
	for (const Config& cfg : configs) {
		if (cfg.vkImageOnly && kind != GfxKind::Vulkan)
			continue;
		GfxTexture left = s.gfx->Pattern(cfg.w, cfg.h, format, 0);
		GfxTexture right = cfg.sideBySide ? left : s.gfx->Pattern(cfg.w, cfg.h, format, 1);
		GfxTexture depth = s.gfx->Depth(cfg.w, cfg.h, 0.25f);
		s.gfx->Flush();
		Json::Value r;
		if (!left.Made() || !right.Made() || !depth.Made()) {
			r["error"] = "the pattern or depth textures couldn't be made";
			c.Record(cfg.name, r);
			first = false;
			continue;
		}
		const vr::VRTextureBounds_t* bounds[2] = { &cfg.left, &cfg.right };
		for (int frame = 1; frame <= (first ? kFrames : 4); frame++) {
			vr::TrackedDevicePose_t poses[16];
			s.comp->WaitGetPoses(poses, 16, nullptr, 0);
			vr::HmdMatrix34_t head = poses[0].mDeviceToAbsoluteTracking;
			if (first) {
				r["head"] = Json::Value(Json::arrayValue);
				for (int i = 0; i < 3; i++)
					r["head"].append(head.m[i][3]);
			}
			head.m[0][3] -= 0.5f;
			GfxTexture* eyes[2] = { &left, &right };
			for (int eye = 0; eye < 2; eye++) {
				vr::VRTextureWithPoseAndDepth_t t{};
				static_cast<vr::Texture_t&>(t) = *eyes[eye]->Texture();
				t.mDeviceToAbsoluteTracking = head;
				t.depth.handle = depth.Texture()->handle;
				vr::VRVulkanTextureData_t imageOnly{};
				if (cfg.vkImageOnly) {
					const auto* full = (const vr::VRVulkanTextureData_t*)depth.Texture()->handle;
					imageOnly.m_nImage = full->m_nImage;
					imageOnly.m_nWidth = full->m_nWidth;
					imageOnly.m_nHeight = full->m_nHeight;
					imageOnly.m_nFormat = full->m_nFormat;
					imageOnly.m_nSampleCount = full->m_nSampleCount;
					t.depth.handle = &imageOnly;
				}
				t.depth.mProjection = projection;
				t.depth.vRange = { 0, 1 };
				r[eye ? "right" : "left"] = (int)s.comp->Submit(eye ? vr::Eye_Right : vr::Eye_Left, &t, bounds[eye],
				    (vr::EVRSubmitFlags)(vr::Submit_TextureWithPose | vr::Submit_TextureWithDepth));
			}
		}
		r["dump_join"] = RectJoin(cfg.rectW, cfg.rectH);
		c.Record(cfg.name, r);
		first = false;
	}
}

// A skybox set while the game stalls (D3D11): count textures of 256x256 (1: lat-long, 2: a stereo pair, 6: cube faces;
// texture i the pattern of eye i % 2), then 300 ms without frames, in which OpenLamina shows it in frames of its own.
// Records the answers, and from the dump the frames that showed it and the last one's layers.
void Skybox(CaseContext& c, uint32_t count)
{
	Scene s(c);
	if (!s.Start(GfxKind::D3D11, "skybox"))
		return;
	GfxTexture left = s.gfx->Pattern(512, 512, kDxgiRGBA8Srgb, 0);
	GfxTexture right = s.gfx->Pattern(512, 512, kDxgiRGBA8Srgb, 1);
	std::vector<GfxTexture> sky;
	for (uint32_t i = 0; i < count; i++)
		sky.push_back(s.gfx->Pattern(256, 256, kDxgiRGBA8Srgb, (int)(i % 2)));
	s.gfx->Flush();
	std::vector<vr::Texture_t> textures;
	for (GfxTexture& t : sky)
		textures.push_back(*t.Texture());

	Json::Value r;
	s.Frames(5, left, right, kFull, kFull, r);
	r["set"] = (int)s.comp->SetSkyboxOverride(textures.data(), count);
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	s.comp->ClearSkyboxOverride();
	s.Frames(2, left, right, kFull, kFull, r);
	r.removeMember("left");
	r.removeMember("right");
	s.c.core->Cleanup();
	s.started = false;

	Json::Value dump = ReadStandInDump();
	if (!dump.isNull()) {
		std::map<int64_t, Json::Value> skyFrames;
		for (const Json::Value& layer : dump["layers"]) {
			std::string type = layer["type"].asString();
			if (type == "equirect2" || type == "cube" || type == "quad")
				skyFrames[layer["frame"].asInt64()].append(layer);
		}
		r["frames_with_skybox_at_least_10"] = skyFrames.size() >= 10;
		if (!skyFrames.empty()) {
			Json::Value last;
			for (const Json::Value& layer : skyFrames.rbegin()->second) {
				Json::Value l;
				l["type"] = layer["type"];
				l["eye_visibility"] = layer["eye_visibility"];
				l["rect"] = layer["rect"];
				l["tl"] = layer["samples"]["tl"];
				l["invalid"] = layer["invalid"];
				last.append(l);
			}
			r["last"] = last;
		}
	}
	c.Record("skybox", r);
}

#ifdef _WIN32
// The top-left pixel of a D3D11 shader resource view's texture, as 0xRRGGBBAA (0 if it can't be read)
uint32_t TopLeftOfView(ID3D11ShaderResourceView* view)
{
	ID3D11Resource* res = nullptr;
	view->GetResource(&res);
	ID3D11Texture2D* tex = nullptr;
	res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
	res->Release();
	if (!tex)
		return 0;
	ID3D11Device* dev = nullptr;
	tex->GetDevice(&dev);
	D3D11_TEXTURE2D_DESC desc;
	tex->GetDesc(&desc);
	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	desc.MiscFlags = 0;
	ID3D11Texture2D* staging = nullptr;
	uint32_t colour = 0;
	if (SUCCEEDED(dev->CreateTexture2D(&desc, nullptr, &staging))) {
		ID3D11DeviceContext* ctx = nullptr;
		dev->GetImmediateContext(&ctx);
		ctx->CopyResource(staging, tex);
		D3D11_MAPPED_SUBRESOURCE map;
		if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
			const uint8_t* px = (const uint8_t*)map.pData + map.RowPitch * 2 + 2 * 4;
			colour = (uint32_t)px[0] << 24 | (uint32_t)px[1] << 16 | (uint32_t)px[2] << 8 | px[3];
			ctx->Unmap(staging, 0);
		}
		ctx->Release();
		staging->Release();
	}
	dev->Release();
	tex->Release();
	return colour;
}

// A COM object's reference count
ULONG References(IUnknown* object)
{
	object->AddRef();
	return object->Release();
}

// The mirror window and the D3D11 mirror texture (expect/compositor.json): shown and hidden. The game's own textures
// are never referenced. A mirror texture asked for before any frame was copied is given at once. A view of the left
// eye's, asked for after a frame and held, shows that frame, and then the next one, of another pattern: the texture
// follows the game's frames. It's given for the device and for one of its resources.
void Mirror(CaseContext& c)
{
	Scene s(c);
	if (!s.Start(GfxKind::D3D11, "mirror"))
		return;
	GfxTexture left = s.gfx->Pattern(256, 256, kDxgiRGBA8Srgb, 0);
	GfxTexture right = s.gfx->Pattern(256, 256, kDxgiRGBA8Srgb, 1);
	s.gfx->Flush();
	Json::Value r;
	s.comp->ShowMirrorWindow();
	r["visible"] = s.comp->IsMirrorWindowVisible();
	auto* leftTex = (ID3D11Texture2D*)left.tex.handle;
	const ULONG before = References(leftTex);
	s.Frames(5, left, right, kFull, kFull, r);

	ID3D11Device* device = nullptr;
	leftTex->GetDevice(&device);
	ID3D11ShaderResourceView* early = nullptr;
	r["first_get"] = (int)s.comp->GetMirrorTextureD3D11(vr::Eye_Left, device, (void**)&early);
	r["first_get_gave_a_view"] = early != nullptr;
	if (early)
		s.comp->ReleaseMirrorTextureD3D11(early);

	// A view asked for after a frame, and held
	s.Frames(2, left, right, kFull, kFull, r);
	ID3D11ShaderResourceView* view = nullptr;
	r["get"] = (int)s.comp->GetMirrorTextureD3D11(vr::Eye_Left, device, (void**)&view);
	auto topLeft = [&] {
		char hex[9] = "";
		if (view)
			snprintf(hex, sizeof(hex), "%08x", TopLeftOfView(view));
		return std::string(hex);
	};
	r["left_top_left"] = topLeft();
	// The other eye's pattern as the left eye's frames: the held view shows it
	s.Frames(2, right, left, kFull, kFull, r);
	r["left_top_left_after_other_frames"] = topLeft();
	r.removeMember("left");
	r.removeMember("right");
	if (view)
		s.comp->ReleaseMirrorTextureD3D11(view);
	view = nullptr;
	r["get_with_resource"] = (int)s.comp->GetMirrorTextureD3D11(vr::Eye_Left, leftTex, (void**)&view);
	if (view)
		s.comp->ReleaseMirrorTextureD3D11(view);
	device->Release();
	r["game_textures_never_referenced"] = References(leftTex) == before;

	s.comp->HideMirrorWindow();
	r["hidden"] = !s.comp->IsMirrorWindowVisible();
	c.Record("mirror", r);
}
#endif

// Overlays from image files and the texture getters (D3D11, expect/gfx.json): a PNG of the pattern in a non-ASCII
// folder shown on an overlay, a damaged file refused, and each overlay's texture and size answered
void OverlayImages(CaseContext& c)
{
	Scene s(c);
	if (!s.Start(GfxKind::D3D11, "images"))
		return;
	vr::EVRInitError le = vr::VRInitError_None;
	auto* ov = (vr::IVROverlay_028::IVROverlay*)c.core->GetGenericInterface("IVROverlay_028", &le);
	GfxTexture left = s.gfx->Pattern(256, 256, kDxgiRGBA8Srgb, 0);
	GfxTexture right = s.gfx->Pattern(256, 256, kDxgiRGBA8Srgb, 1);
	GfxTexture texture = s.gfx->Pattern(128, 64, kDxgiRGBA8Srgb, 0);
	s.gfx->Flush();
	if (!ov) {
		c.Record("no overlay");
		return;
	}

	// The PNG, written here: 64x64 of the pattern
	std::vector<uint8_t> pattern = PatternBytes(64, 64);
	std::vector<uint8_t> png = test_png::Encode(pattern.data(), 64, 64);
	std::filesystem::path folder = std::filesystem::temp_directory_path() / std::filesystem::path(std::u8string(u8"oc_overlay_images_J\u00fcrgen"));
	std::filesystem::create_directories(folder);
	std::filesystem::path good = folder / "pattern.png", bad = folder / "damaged.png";
	std::ofstream(good, std::ios::binary).write((const char*)png.data(), (std::streamsize)png.size());
	std::ofstream(bad, std::ios::binary).write((const char*)png.data(), (std::streamsize)(png.size() / 2));

	Json::Value r;
	vr::VROverlayHandle_t file = vr::k_ulOverlayHandleInvalid, tex = vr::k_ulOverlayHandleInvalid;
	ov->CreateOverlay("oc.test.file", "File", &file);
	ov->CreateOverlay("oc.test.tex", "Texture", &tex);
	auto utf8 = [](const std::filesystem::path& p) {
		std::u8string u = p.u8string();
		return std::string(u.begin(), u.end());
	};
	r["file"] = (int)ov->SetOverlayFromFile(file, utf8(good).c_str());
	r["damaged"] = (int)ov->SetOverlayFromFile(tex, utf8(bad).c_str());
	r["texture"] = (int)ov->SetOverlayTexture(tex, texture.Texture());
	ov->ShowOverlay(file);

	uint32_t w = 0, h = 0;
	r["file_size_ret"] = (int)ov->GetOverlayTextureSize(file, &w, &h);
	r["file_size"].append(w);
	r["file_size"].append(h);
	w = h = 0;
	r["tex_size_ret"] = (int)ov->GetOverlayTextureSize(tex, &w, &h);
	r["tex_size"].append(w);
	r["tex_size"].append(h);
	void* handle = nullptr;
	vr::ETextureType type = vr::TextureType_Invalid;
	vr::EColorSpace space = vr::ColorSpace_Auto;
	vr::VRTextureBounds_t bounds{};
	uint32_t format = 0;
	r["file_texture"] = (int)ov->GetOverlayTexture(file, &handle, nullptr, &w, &h, &format, &type, &space, &bounds);
	// A D3D11 owner gets a shader resource view of its texture, on the device of the texture it names
	r["tex_texture"] = (int)ov->GetOverlayTexture(tex, &handle, texture.tex.handle, &w, &h, &format, &type, &space, &bounds);
	r["tex_format"] = format;
	bool isView = false;
#ifdef _WIN32
	if (handle) {
		ID3D11ShaderResourceView* view = nullptr;
		if (SUCCEEDED(((IUnknown*)handle)->QueryInterface(__uuidof(ID3D11ShaderResourceView), (void**)&view))) {
			ID3D11Resource* of = nullptr;
			view->GetResource(&of);
			isView = of == (ID3D11Resource*)texture.tex.handle;
			if (of)
				of->Release();
			view->Release();
		}
	}
#endif
	r["tex_texture_is_a_view_of_it"] = isView && type == vr::TextureType_DirectX;
	r["release"] = (int)ov->ReleaseNativeOverlayHandle(tex, handle);
	r["release_again"] = (int)ov->ReleaseNativeOverlayHandle(tex, handle);

	s.Frames(5, left, right, kFull, kFull, r);
	r.removeMember("left");
	r.removeMember("right");
	r["dump_join"] = "last_layer:quad";
	c.Record("images", r);
	ov->DestroyOverlay(file);
	ov->DestroyOverlay(tex);
	std::error_code ec;
	std::filesystem::remove_all(folder, ec);
}

} // namespace

const std::map<std::string, CaseFn>& conformance::GfxCases()
{
	static const std::map<std::string, CaseFn> cases = {
		{ "gfx-d3d11-basic", [](CaseContext& c) { Basic(c, GfxKind::D3D11, kDxgiRGBA8Srgb); } },
		{ "overlay-images", OverlayImages },
		{ "skybox-1", [](CaseContext& c) { Skybox(c, 1); } },
		{ "skybox-2", [](CaseContext& c) { Skybox(c, 2); } },
		{ "skybox-6", [](CaseContext& c) { Skybox(c, 6); } },
#ifdef _WIN32
		{ "mirror", Mirror },
#endif
		{ "gfx-d3d12-basic", [](CaseContext& c) { Basic(c, GfxKind::D3D12, kDxgiRGBA8Srgb); } },
		{ "gfx-vk-basic", [](CaseContext& c) { Basic(c, GfxKind::Vulkan, kVkRGBA8Srgb); } },
		{ "gfx-gl-basic", [](CaseContext& c) { Basic(c, GfxKind::OpenGL, kGlSRGB8Alpha8); } },
		{ "gfx-vk-dump-images", [](CaseContext& c) { Basic(c, GfxKind::Vulkan, kVkRGBA8Srgb, true); } },
		{ "gfx-vk-wait-without-submit", [](CaseContext& c) { WaitWithoutSubmit(c, GfxKind::Vulkan, kVkRGBA8Srgb); } },
		{ "gfx-gl-wait-without-submit", [](CaseContext& c) { WaitWithoutSubmit(c, GfxKind::OpenGL, kGlSRGB8Alpha8); } },
		{ "gfx-gl-dump-images", [](CaseContext& c) { Basic(c, GfxKind::OpenGL, kGlSRGB8Alpha8, true); } },
		{ "gfx-d3d11-formats", [](CaseContext& c) { Formats(c, GfxKind::D3D11, { { "rgba16f", 10 }, { "r11g11b10f", 26 }, { "rgb10a2", 24 }, { "bgrx8", 88 } }); } },
		{ "gfx-d3d12-formats", [](CaseContext& c) { Formats(c, GfxKind::D3D12, { { "rgba16f", 10 }, { "r11g11b10f", 26 }, { "rgb10a2", 24 }, { "bgrx8", 88 } }); } },
		{ "gfx-vk-formats", [](CaseContext& c) { Formats(c, GfxKind::Vulkan, { { "rgba16f", 97 }, { "r11g11b10f", 122 }, { "rgb10a2", 64 }, { "bgra8", 44 } }); } },
		{ "gfx-gl-formats", [](CaseContext& c) { Formats(c, GfxKind::OpenGL, { { "rgba16f", 0x881A }, { "r11g11b10f", 0x8C3A }, { "rgb10a2", 0x8059 }, { "rgb8", 0x8051 } }); } },
		{ "gfx-d3d11-odd", D3D11Odd },
		{ "gfx-vk-odd", VkOdd },
		{ "gfx-vk-dynamic-resolution", VkDynamicResolution },
		{ "gfx-vk-msaa-formats", [](CaseContext& c) { Formats(c, GfxKind::Vulkan, { { "msaa bgra8", 44 } }, 4); } },
		{ "gfx-d3d11-new-device", [](CaseContext& c) { NewDevice(c, GfxKind::D3D11, kDxgiRGBA8Srgb); } },
		{ "gfx-d3d12-new-device", [](CaseContext& c) { NewDevice(c, GfxKind::D3D12, kDxgiRGBA8Srgb); } },
		{ "gfx-vk-new-device", [](CaseContext& c) { NewDevice(c, GfxKind::Vulkan, kVkRGBA8Srgb); } },
		{ "gfx-d3d11-eyes-two-devices", EyesOnTwoDevices },
		{ "gfx-d3d11-pose-depth", [](CaseContext& c) { PoseDepth(c, GfxKind::D3D11, kDxgiRGBA8Srgb); } },
		{ "gfx-d3d12-pose-depth", [](CaseContext& c) { PoseDepth(c, GfxKind::D3D12, kDxgiRGBA8Srgb); } },
		{ "gfx-vk-pose-depth", [](CaseContext& c) { PoseDepth(c, GfxKind::Vulkan, kVkRGBA8Srgb); } },
		{ "gfx-gl-pose-depth", [](CaseContext& c) { PoseDepth(c, GfxKind::OpenGL, kGlSRGB8Alpha8); } },
		{ "gfx-d3d11-bounds", [](CaseContext& c) { Bounds(c, GfxKind::D3D11, kDxgiRGBA8Srgb); } },
		{ "gfx-d3d12-bounds", [](CaseContext& c) { Bounds(c, GfxKind::D3D12, kDxgiRGBA8Srgb); } },
		{ "gfx-vk-bounds", [](CaseContext& c) { Bounds(c, GfxKind::Vulkan, kVkRGBA8Srgb); } },
		{ "gfx-gl-bounds", [](CaseContext& c) { Bounds(c, GfxKind::OpenGL, kGlSRGB8Alpha8); } },
		{ "overlay-raw-d3d11", [](CaseContext& c) { OverlayRaw(c, GfxKind::D3D11, kDxgiRGBA8Srgb); } },
		{ "overlay-raw-d3d12", [](CaseContext& c) { OverlayRaw(c, GfxKind::D3D12, kDxgiRGBA8Srgb); } },
		{ "overlay-raw-vk", [](CaseContext& c) { OverlayRaw(c, GfxKind::Vulkan, kVkRGBA8Srgb); } },
		{ "overlay-raw-gl", [](CaseContext& c) { OverlayRaw(c, GfxKind::OpenGL, kGlSRGB8Alpha8); } },
		{ "overlay-texture-readback-d3d11", [](CaseContext& c) { OverlayTextureReadback(c, GfxKind::D3D11, kDxgiRGBA8Srgb); } },
		{ "overlay-texture-readback-d3d12", [](CaseContext& c) { OverlayTextureReadback(c, GfxKind::D3D12, kDxgiRGBA8Srgb); } },
		{ "overlay-texture-readback-vk", [](CaseContext& c) { OverlayTextureReadback(c, GfxKind::Vulkan, kVkRGBA8Srgb); } },
		{ "overlay-texture-readback-gl", [](CaseContext& c) { OverlayTextureReadback(c, GfxKind::OpenGL, kGlSRGB8Alpha8); } },
		{ "gfx-d3d10-basic", [](CaseContext& c) { Basic(c, GfxKind::D3D10, kDxgiRGBA8Srgb); } },
		// As the basic ones, under a script whose runtime has D3D11 alone (expect/gfx_interop.json)
		{ "gfx-gl-interop", [](CaseContext& c) { Basic(c, GfxKind::OpenGL, kGlSRGB8Alpha8); } },
		{ "gfx-gl-interop-bounds", [](CaseContext& c) { Bounds(c, GfxKind::OpenGL, kGlSRGB8Alpha8); } },
		{ "gfx-vk-interop-bounds", [](CaseContext& c) { Bounds(c, GfxKind::Vulkan, kVkRGBA8Srgb); } },
		{ "gfx-vk-interop", [](CaseContext& c) { Basic(c, GfxKind::Vulkan, kVkRGBA8Srgb); } },
	};
	return cases;
}
