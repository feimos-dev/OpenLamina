#include "doctest.h"

#include "stdafx.h"

#include "XrHMD.h"
#include "Reimpl/BaseSystem.h"

#ifndef _WIN32
#include "steamvr_abi.h"

// Linking the HMD pulls in the client core, which calls the OpenVR entry points that only the vrclient library
// itself defines. Nothing here reaches them.
VR_INTERFACE void* VR_CALLTYPE VR_GetGenericInterface(const char*, vr::EVRInitError*) { return nullptr; }
VR_INTERFACE const char* VR_CALLTYPE VR_GetVRInitErrorAsEnglishDescription(vr::EVRInitError) { return ""; }
VR_INTERFACE const char* VR_CALLTYPE VR_GetVRInitErrorAsSymbol(vr::EVRInitError) { return ""; }
VR_INTERFACE uint32_t VR_CALLTYPE VR_InitInternal2(vr::EVRInitError*, vr::EVRApplicationType, const char*) { return 0; }
VR_INTERFACE bool VR_CALLTYPE VR_IsHmdPresent() { return false; }
VR_INTERFACE bool VR_CALLTYPE VR_IsInterfaceVersionValid(const char*) { return false; }
VR_INTERFACE void VR_CALLTYPE VR_ShutdownInternal() {}
#endif

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
