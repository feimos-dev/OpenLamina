#include "doctest.h"

#include "stdafx.h"

#include "XrHMD.h"
#include "Reimpl/BaseSystem.h"
#include "logging.h"

#include <cmath>
#include <string>
#include <vector>

TEST_CASE("The IPD before the runtime has views is a human one")
{
	// Without a session there are no views to measure
	XrHMD hmd;
	CHECK(hmd.GetIPD() == doctest::Approx(0.064f));
}

#include "Drivers/Backend.h"
#include "Reimpl/BaseExtendedDisplay.h"
#include "Reimpl/BaseHeadsetView.h"

namespace {

// An HMD that answers a render target size
class SizedHmd : public IHMD {
public:
	uint32_t width = 0, height = 0;

	void GetPose(vr::ETrackingUniverseOrigin, vr::TrackedDevicePose_t*, ETrackingStateType) override {}
	void GetPose(vr::ETrackingUniverseOrigin, vr::TrackedDevicePose_t*, ETrackingStateType, double) override {}
	void GetRecommendedRenderTargetSize(uint32_t* w, uint32_t* h) override
	{
		*w = width;
		*h = height;
	}
	vr::HmdMatrix44_t GetProjectionMatrix(vr::EVREye, float, float, EGraphicsAPIConvention) override { return {}; }
	void GetProjectionRaw(vr::EVREye, float*, float*, float*, float*) override {}
	bool ComputeDistortion(vr::EVREye, float, float, vr::DistortionCoordinates_t*) override { return false; }
	vr::HmdMatrix34_t GetEyeToHeadTransform(vr::EVREye) override { return {}; }
	float GetIPD() override { return 0.064f; }
	bool GetTimeSinceLastVsync(float*, uint64_t*) override { return false; }
	vr::HiddenAreaMesh_t GetHiddenAreaMesh(vr::EVREye, vr::EHiddenAreaMeshType) override { return {}; }
};

class SizedBackend : public IBackend {
public:
	std::shared_ptr<SizedHmd> hmd = std::make_shared<SizedHmd>();

	std::shared_ptr<IHMD> GetPrimaryHMD() override { return hmd; }
	std::shared_ptr<ITrackedDevice> GetDevice(vr::TrackedDeviceIndex_t) override { return nullptr; }
	std::shared_ptr<ITrackedDevice> GetDeviceByHand(ITrackedDevice::TrackedDeviceType) override { return nullptr; }
	void GetDeviceToAbsoluteTrackingPose(vr::ETrackingUniverseOrigin, float, vr::TrackedDevicePose_t*, uint32_t) override {}
	void WaitForTrackingData() override {}
	IBackend::openvr_enum_t StoreEyeTexture(vr::EVREye, const vr::Texture_t*, const vr::VRTextureBounds_t*, vr::EVRSubmitFlags, bool) override { return 0; }
	void SubmitFrames(bool, bool) override {}
	IBackend::openvr_enum_t SetSkyboxOverride(const vr::Texture_t*, uint32_t) override { return 0; }
	void ClearSkyboxOverride() override {}
	bool GetFrameTiming(OOVR_Compositor_FrameTiming*, uint32_t) override { return false; }
	IBackend::openvr_enum_t GetMirrorTextureD3D11(vr::EVREye, void*, void**) override { return 0; }
	void ReleaseMirrorTextureD3D11(void*) override {}
	bool GetPlayAreaPoints(vr::HmdVector3_t*, int*) override { return false; }
	bool AreBoundsVisible() override { return false; }
	void ForceBoundsVisible(bool) override {}
	void PumpEvents() override {}
	bool IsInputAvailable() override { return false; }
	bool IsGraphicsConfigured() override { return false; }
	void OnOverlayTexture(const vr::Texture_t*) override {}
};

// A backend whose HMD has a 1000 x 1200 eye, for the length of the test
struct WithEyeSize {
	SizedBackend* backend = new SizedBackend();
	WithEyeSize(uint32_t w, uint32_t h)
	{
		backend->hmd->width = w;
		backend->hmd->height = h;
		BackendManager::Create(backend);
	}
	~WithEyeSize() { BackendManager::Reset(); }
};

}

TEST_CASE("The extended display's window is two eyes wide and one eye tall")
{
	WithEyeSize eyes(1000, 1200);
	BaseExtendedDisplay display;

	int32_t x = -1, y = -1;
	uint32_t w = 0, h = 0;
	display.GetWindowBounds(&x, &y, &w, &h);
	CHECK(x == 0);
	CHECK(y == 0);
	CHECK(w == 2000);
	CHECK(h == 1200);
}

TEST_CASE("The extended display's eye viewports tile the window")
{
	WithEyeSize eyes(1000, 1200);
	BaseExtendedDisplay display;

	uint32_t x, y, w, h;
	display.GetEyeOutputViewport(vr::Eye_Left, &x, &y, &w, &h);
	CHECK(x == 0);
	CHECK(y == 0);
	CHECK(w == 1000);
	CHECK(h == 1200);
	display.GetEyeOutputViewport(vr::Eye_Right, &x, &y, &w, &h);
	CHECK(x == 1000);
	CHECK(w == 1000);
	CHECK(h == 1200);
	// Outputs the caller doesn't want
	display.GetEyeOutputViewport(vr::Eye_Left, nullptr, nullptr, nullptr, nullptr);
}

TEST_CASE("The headset view reads back what was set")
{
	WithEyeSize eyes(1000, 1250);
	BaseHeadsetView view;

	view.SetHeadsetViewSize(800, 600);
	uint32_t w = 0, h = 0;
	view.GetHeadsetViewSize(&w, &h);
	CHECK(w == 800);
	CHECK(h == 600);

	// Clamped to the largest size
	view.SetHeadsetViewSize(100000, 100000);
	view.GetHeadsetViewSize(&w, &h);
	CHECK(w == 3840u);
	CHECK(h == 2160u);

	view.SetHeadsetViewMode(HeadsetViewMode_Right);
	CHECK(view.GetHeadsetViewMode() == HeadsetViewMode_Right);
	view.SetHeadsetViewMode((OOVR_HeadsetViewMode_t)77);
	CHECK(view.GetHeadsetViewMode() == HeadsetViewMode_Right);

	view.SetHeadsetViewCropped(true);
	CHECK(view.GetHeadsetViewCropped());

	view.SetHeadsetViewBlendRange(0.25f, 3.0f);
	float start = -1, end = -1;
	view.GetHeadsetViewBlendRange(&start, &end);
	CHECK(start == doctest::Approx(0.25f));
	CHECK(end == doctest::Approx(1.0f));
}

TEST_CASE("The headset view's aspect ratio follows the eye and the mode")
{
	WithEyeSize eyes(1000, 1250);
	BaseHeadsetView view;

	view.SetHeadsetViewMode(HeadsetViewMode_Left);
	CHECK(view.GetHeadsetViewAspectRatio() == doctest::Approx(0.8f));
	view.SetHeadsetViewMode(HeadsetViewMode_Both);
	CHECK(view.GetHeadsetViewAspectRatio() == doctest::Approx(1.6f));
}

TEST_CASE("Distortion comes back as given")
{
	XrHMD hmd;
	vr::DistortionCoordinates_t out = {};
	CHECK(hmd.ComputeDistortion(vr::Eye_Left, 0.25f, 0.75f, &out));
	CHECK(out.rfRed[0] == 0.25f);
	CHECK(out.rfGreen[0] == 0.25f);
	CHECK(out.rfBlue[0] == 0.25f);
	CHECK(out.rfRed[1] == 0.75f);
	CHECK(out.rfGreen[1] == 0.75f);
	CHECK(out.rfBlue[1] == 0.75f);
	CHECK_FALSE(hmd.ComputeDistortion(vr::Eye_Left, 0.25f, 0.75f, nullptr));

	BaseSystem system;
	vr::DistortionCoordinate_t in[2] = { { 0.1f, 0.2f }, { 0.3f, 0.4f } }, set[2] = {};
	CHECK(system.ComputeDistortionSet(vr::Eye_Right, vr::EVRDistortionChannel::Blue, false, 2, in, set));
	CHECK(set[1].u == 0.3f);
	CHECK(set[1].v == 0.4f);
	CHECK_FALSE(system.ComputeDistortionSet(vr::Eye_Right, vr::EVRDistortionChannel::Blue, false, 2, nullptr, set));
}

#ifndef SUPPORT_DX
TEST_CASE("Without D3D the adapter queries answer -1")
{
	BaseSystem system;
	CHECK(system.GetD3D9AdapterIndex() == -1);

	int32_t adapter = 7, output = 7;
	system.GetDXGIOutputInfo(&adapter, &output);
	CHECK(adapter == -1);
	CHECK(output == -1);

	adapter = output = 7;
	BaseExtendedDisplay().GetDXGIOutputInfo(&adapter, &output);
	CHECK(adapter == -1);
	CHECK(output == -1);
}
#endif

TEST_CASE("A time's place on the vsync grid")
{
	const XrDuration period = 10000000; // 10 ms
	float since = -1;
	uint64_t count = 99;

	// Ahead of the anchor by 2.5 periods
	CHECK(XrHMD::VsyncPhase(1000000000 + 25000000, 1000000000, period, &since, &count));
	CHECK(since == doctest::Approx(0.005f));
	CHECK(count == 100 + 2);

	// Before the anchor: the vsync before it, not after
	CHECK(XrHMD::VsyncPhase(1000000000 - 3000000, 1000000000, period, &since, &count));
	CHECK(since == doctest::Approx(0.007f));
	CHECK(count == 100 - 1);

	// On a vsync
	CHECK(XrHMD::VsyncPhase(1000000000 + 30000000, 1000000000, period, &since, &count));
	CHECK(since == doctest::Approx(0.0f));
	CHECK(count == 103);

	// No period, no anchor or no clock: no answer, and the outputs are written
	since = -1;
	count = 99;
	CHECK_FALSE(XrHMD::VsyncPhase(1000000000, 1000000000, 0, &since, &count));
	CHECK(since == 0.0f);
	CHECK(count == 0);
	CHECK_FALSE(XrHMD::VsyncPhase(0, 1000000000, period, &since, &count));
	CHECK_FALSE(XrHMD::VsyncPhase(1000000000, 1, period, &since, &count));
	CHECK(XrHMD::VsyncPhase(1000000000, 1000000000, period, nullptr, nullptr));
}

TEST_CASE("Without a session the vsync time is unknown and still written")
{
	XrHMD hmd;
	float since = 5.0f;
	uint64_t count = 5;
	CHECK_FALSE(hmd.GetTimeSinceLastVsync(&since, &count));
	CHECK(since == 0.0f);
	CHECK(count == 0);
	CHECK_FALSE(hmd.GetTimeSinceLastVsync(nullptr, nullptr));
}

namespace {
std::vector<std::string> logged;
void keepLine(const char* line) { logged.emplace_back(line); }
}

TEST_CASE("The eye relief property is zero without a complaint")
{
	logged.clear();
	oovr_log_set_test_sink(keepLine);
	XrHMD hmd;
	vr::ETrackedPropertyError err = vr::TrackedProp_UnknownProperty;
	float depth = hmd.GetFloatTrackedDeviceProperty(vr::Prop_UserHeadToEyeDepthMeters_Float, &err);
	oovr_log_set_test_sink(nullptr);

	CHECK(depth == 0.0f);
	CHECK(err == vr::TrackedProp_Success);
	CHECK(logged.empty());
}

TEST_CASE("Scaled image sizes stay within what the runtime allows")
{
	CHECK(XrHMD::ScaledImageSize(1000, 4000, 1.0f) == 1000);
	CHECK(XrHMD::ScaledImageSize(1000, 4000, 1.5f) == 1500);
	// Rounded, not cut off
	CHECK(XrHMD::ScaledImageSize(1001, 4000, 0.9995f) == 1000);
	CHECK(XrHMD::ScaledImageSize(999, 4000, 1.0006f) == 1000);
	// Clamped to the largest
	CHECK(XrHMD::ScaledImageSize(2000, 3000, 2.0f) == 3000);
	// No limit given, no scale given
	CHECK(XrHMD::ScaledImageSize(2000, 0, 2.0f) == 4000);
	CHECK(XrHMD::ScaledImageSize(2000, 4000, 0.0f) == 2000);
	// Never nothing for a view that has a size
	CHECK(XrHMD::ScaledImageSize(10, 4000, 0.001f) == 1);
	CHECK(XrHMD::ScaledImageSize(0, 4000, 1.0f) == 0);
}

namespace {
// The row-major matrix applied to a view-space point, as the graphics API sees clip space
void ToClip(const vr::HmdMatrix44_t& m, float x, float y, float z, float out[4])
{
	for (int r = 0; r < 4; r++)
		out[r] = m.m[r][0] * x + m.m[r][1] * y + m.m[r][2] * z + m.m[r][3];
}
}

TEST_CASE("The projection maps the near and far planes to the API's depth range")
{
	const float nearZ = 0.1f, farZ = 50.0f;
	// An off-axis view: 50 degrees left, 40 right, 30 up, 45 down
	const float tl = std::tan(-0.8727f), tr = std::tan(0.6981f), tu = std::tan(0.5236f), td = std::tan(-0.7854f);
	float c[4];

	vr::HmdMatrix44_t gl = XrHMD::ProjectionFromTangents(tl, tr, tu, td, nearZ, farZ, API_OpenGL);
	ToClip(gl, 0, 0, -nearZ, c);
	CHECK(c[2] / c[3] == doctest::Approx(-1.0f).epsilon(1e-4));
	ToClip(gl, 0, 0, -farZ, c);
	CHECK(c[2] / c[3] == doctest::Approx(1.0f).epsilon(1e-4));

	vr::HmdMatrix44_t dx = XrHMD::ProjectionFromTangents(tl, tr, tu, td, nearZ, farZ, API_DirectX);
	ToClip(dx, 0, 0, -nearZ, c);
	CHECK(c[2] / c[3] == doctest::Approx(0.0f).epsilon(1e-4));
	ToClip(dx, 0, 0, -farZ, c);
	CHECK(c[2] / c[3] == doctest::Approx(1.0f).epsilon(1e-4));

	// The view's edges at distance 1 land on the edges of the screen, in both
	for (const vr::HmdMatrix44_t* m : { &gl, &dx }) {
		ToClip(*m, tl, 0, -1, c);
		CHECK(c[0] / c[3] == doctest::Approx(-1.0f).epsilon(1e-4));
		ToClip(*m, tr, 0, -1, c);
		CHECK(c[0] / c[3] == doctest::Approx(1.0f).epsilon(1e-4));
		ToClip(*m, 0, tu, -1, c);
		CHECK(c[1] / c[3] == doctest::Approx(1.0f).epsilon(1e-4));
		ToClip(*m, 0, td, -1, c);
		CHECK(c[1] / c[3] == doctest::Approx(-1.0f).epsilon(1e-4));
	}
}

TEST_CASE("Fields of view the runtime gives are checked before use")
{
	CHECK(XrHMD::IsValidFov({ -0.8f, 0.7f, 0.6f, -0.7f }));
	CHECK_FALSE(XrHMD::IsValidFov({ 0, 0, 0, 0 }));
	CHECK_FALSE(XrHMD::IsValidFov({ -0.8f, 0.7f, 0, 0 }));
	CHECK_FALSE(XrHMD::IsValidFov({ 0.7f, -0.8f, 0.6f, -0.7f }));
	CHECK_FALSE(XrHMD::IsValidFov({ -1.6f, 0.7f, 0.6f, -0.7f }));
	CHECK_FALSE(XrHMD::IsValidFov({ std::nanf(""), 0.7f, 0.6f, -0.7f }));
}

TEST_CASE("Without views the projection is a finite 90 degree one")
{
	XrHMD hmd;
	vr::HmdMatrix44_t m = hmd.GetProjectionMatrix(vr::Eye_Left, 0.1f, 100.0f, API_OpenGL);
	for (int r = 0; r < 4; r++)
		for (int c = 0; c < 4; c++)
			CHECK(std::isfinite(m.m[r][c]));
	CHECK(m.m[0][0] == doctest::Approx(1.0f));
	CHECK(m.m[1][1] == doctest::Approx(1.0f));

	float l, r, t, b;
	hmd.GetProjectionRaw(vr::Eye_Right, &l, &r, &t, &b);
	CHECK(l == doctest::Approx(-1.0f));
	CHECK(r == doctest::Approx(1.0f));
	CHECK(t == doctest::Approx(-1.0f));
	CHECK(b == doctest::Approx(1.0f));
}
