// The frame cases, gfx-<api>-<what>: a game on one graphics API (gfx/GfxApi.h) submits the test pattern, and the
// stand-in runtime's dump shows what OpenLamina made of it. Each records "frame" with "dump_join":
// "last_projection", for which run.py joins the dump's last projection layer (per view: samples, rect, format,
// invalid) as the record's "dump"; a case whose API can't be made here records {"skipped": why}.

#include "conformance.h"

#include "gfx/GfxApi.h"

#include "IVRCompositor_029.h"
#include "IVROverlay_028.h"

#include <algorithm>
#include <thread>
#include <map>
#include <chrono>
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
// a script whose runtime offers RGBA8 sRGB only: each is converted into it
void Formats(CaseContext& c, GfxKind kind, const std::vector<std::pair<std::string, int64_t>>& formats)
{
	Scene s(c);
	if (!s.Start(kind, "format " + formats[0].first))
		return;
	uint32_t size = 512;
	bool first = true;
	for (const auto& [name, format] : formats) {
		GfxTexture left = s.gfx->Pattern(size, size, format, 0);
		GfxTexture right = s.gfx->Pattern(size, size, format, 1);
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

// The pose a frame was rendered with and its depth: each eye's texture (512 x 512) goes with a head pose 0.5 m to the left
// of the one WaitGetPoses gave, and a depth texture of 0.25 everywhere from a projection with near 0.1 and far 100
// (Submit_TextureWithPose | Submit_TextureWithDepth). Records the located head's position as "head".
void PoseDepth(CaseContext& c, GfxKind kind, int64_t format)
{
	Scene s(c);
	if (!s.Start(kind, "pose-depth"))
		return;
	GfxTexture left = s.gfx->Pattern(512, 512, format, 0);
	GfxTexture right = s.gfx->Pattern(512, 512, format, 1);
	GfxTexture depth = s.gfx->Depth(512, 512, 0.25f);
	s.gfx->Flush();
	Json::Value r;
	if (!left.Made() || !right.Made() || !depth.Made()) {
		r["error"] = "the pattern or depth textures couldn't be made";
		c.Record("pose-depth", r);
		return;
	}
	// OpenVR's projection (D3D convention) for near 0.1 and far 100
	const float n = 0.1f, f = 100.0f;
	vr::HmdMatrix44_t projection{};
	projection.m[0][0] = 1;
	projection.m[1][1] = 1;
	projection.m[2][2] = f / (n - f);
	projection.m[2][3] = n * f / (n - f);
	projection.m[3][2] = -1;
	for (int frame = 1; frame <= kFrames; frame++) {
		vr::TrackedDevicePose_t poses[16];
		s.comp->WaitGetPoses(poses, 16, nullptr, 0);
		vr::HmdMatrix34_t head = poses[0].mDeviceToAbsoluteTracking;
		r["head"] = Json::Value(Json::arrayValue);
		for (int i = 0; i < 3; i++)
			r["head"].append(head.m[i][3]);
		head.m[0][3] -= 0.5f;
		GfxTexture* eyes[2] = { &left, &right };
		for (int eye = 0; eye < 2; eye++) {
			vr::VRTextureWithPoseAndDepth_t t{};
			static_cast<vr::Texture_t&>(t) = *eyes[eye]->Texture();
			t.mDeviceToAbsoluteTracking = head;
			t.depth.handle = depth.Texture()->handle;
			t.depth.mProjection = projection;
			t.depth.vRange = { 0, 1 };
			r[eye ? "right" : "left"] = (int)s.comp->Submit(eye ? vr::Eye_Right : vr::Eye_Left, &t, &kFull,
			    (vr::EVRSubmitFlags)(vr::Submit_TextureWithPose | vr::Submit_TextureWithDepth));
		}
	}
	r["dump_join"] = "last_projection";
	c.Record("pose-depth", r);
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

} // namespace

const std::map<std::string, CaseFn>& conformance::GfxCases()
{
	static const std::map<std::string, CaseFn> cases = {
		{ "gfx-d3d11-basic", [](CaseContext& c) { Basic(c, GfxKind::D3D11, kDxgiRGBA8Srgb); } },
		{ "skybox-1", [](CaseContext& c) { Skybox(c, 1); } },
		{ "skybox-2", [](CaseContext& c) { Skybox(c, 2); } },
		{ "skybox-6", [](CaseContext& c) { Skybox(c, 6); } },
		{ "gfx-d3d12-basic", [](CaseContext& c) { Basic(c, GfxKind::D3D12, kDxgiRGBA8Srgb); } },
		{ "gfx-vk-basic", [](CaseContext& c) { Basic(c, GfxKind::Vulkan, kVkRGBA8Srgb); } },
		{ "gfx-gl-basic", [](CaseContext& c) { Basic(c, GfxKind::OpenGL, kGlSRGB8Alpha8); } },
		{ "gfx-d3d11-formats", [](CaseContext& c) { Formats(c, GfxKind::D3D11, { { "rgba16f", 10 }, { "r11g11b10f", 26 }, { "rgb10a2", 24 }, { "bgrx8", 88 } }); } },
		{ "gfx-d3d12-formats", [](CaseContext& c) { Formats(c, GfxKind::D3D12, { { "rgba16f", 10 }, { "r11g11b10f", 26 }, { "rgb10a2", 24 }, { "bgrx8", 88 } }); } },
		{ "gfx-vk-formats", [](CaseContext& c) { Formats(c, GfxKind::Vulkan, { { "rgba16f", 97 }, { "r11g11b10f", 122 }, { "rgb10a2", 64 }, { "bgra8", 44 } }); } },
		{ "gfx-gl-formats", [](CaseContext& c) { Formats(c, GfxKind::OpenGL, { { "rgba16f", 0x881A }, { "r11g11b10f", 0x8C3A }, { "rgb10a2", 0x8059 }, { "rgb8", 0x8051 } }); } },
		{ "gfx-d3d11-odd", D3D11Odd },
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
		{ "gfx-vk-interop", [](CaseContext& c) { Basic(c, GfxKind::Vulkan, kVkRGBA8Srgb); } },
		{ "gfx-d3d10-bounds", [](CaseContext& c) { Bounds(c, GfxKind::D3D10, kDxgiRGBA8Srgb); } },
	};
	return cases;
}
