#include "doctest.h"

#include "stdafx.h"

#include "Drivers/Backend.h"
#include "Reimpl/BaseSystem.h"

#include <map>
#include <vector>

namespace {

// A device that sits at a fixed place, as a controller of some role, or not there at all
class FakeDevice : public ITrackedDevice {
public:
	vr::ETrackedDeviceClass cls = vr::TrackedDeviceClass_Controller;
	vr::ETrackedControllerRole role = vr::TrackedControllerRole_Invalid;
	bool valid = true;
	float x = 0;

	void GetPose(vr::ETrackingUniverseOrigin, vr::TrackedDevicePose_t* pose, ETrackingStateType, int64_t) override
	{
		*pose = {};
		pose->bDeviceIsConnected = true;
		pose->bPoseIsValid = valid;
		pose->mDeviceToAbsoluteTracking.m[0][0] = pose->mDeviceToAbsoluteTracking.m[1][1] = pose->mDeviceToAbsoluteTracking.m[2][2] = 1;
		pose->mDeviceToAbsoluteTracking.m[0][3] = x;
	}
	vr::ETrackedDeviceClass GetTrackedDeviceClass() override { return cls; }
	vr::ETrackedControllerRole GetControllerRole() override { return role; }
};

class SlotsBackend : public IBackend {
public:
	std::map<vr::TrackedDeviceIndex_t, std::shared_ptr<FakeDevice>> devices;
	bool lost = false;
	bool strained = false;

	std::shared_ptr<IHMD> GetPrimaryHMD() override { return nullptr; }
	std::shared_ptr<ITrackedDevice> GetDevice(vr::TrackedDeviceIndex_t index) override
	{
		auto it = devices.find(index);
		return it == devices.end() ? nullptr : it->second;
	}
	std::shared_ptr<ITrackedDevice> GetDeviceByHand(ITrackedDevice::TrackedDeviceType) override { return nullptr; }
	void GetDeviceToAbsoluteTrackingPose(vr::ETrackingUniverseOrigin, float, vr::TrackedDevicePose_t*, uint32_t) override {}
	void WaitForTrackingData() override {}
	IBackend::openvr_enum_t StoreEyeTexture(vr::EVREye, const vr::Texture_t*, const vr::VRTextureBounds_t*, vr::EVRSubmitFlags, bool) override { return 0; }
	void SubmitFrames(bool) override {}
	void ClearSubmittedFrame() override {}
	void SetRenderingSuspended(bool) override {}
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
	bool IsInputFocusLost() override { return lost; }
	bool ShouldReduceRenderingWork() override { return strained; }
	bool IsGraphicsConfigured() override { return false; }
	void OnOverlayTexture(const vr::Texture_t*) override {}
};

struct WithSlots {
	SlotsBackend* backend = new SlotsBackend();
	WithSlots() { BackendManager::Create(backend); }
	~WithSlots() { BackendManager::Reset(); }

	std::shared_ptr<FakeDevice> Add(vr::TrackedDeviceIndex_t index, vr::ETrackedControllerRole role, float x)
	{
		auto dev = std::make_shared<FakeDevice>();
		dev->role = role;
		dev->x = x;
		backend->devices[index] = dev;
		return dev;
	}
};

}

TEST_CASE("A slot keeps the class of the device it held")
{
	WithSlots slots;
	BaseSystem system;
	slots.Add(40, vr::TrackedControllerRole_RightHand, 0);
	CHECK(system.GetTrackedDeviceClass(40) == vr::TrackedDeviceClass_Controller);
	CHECK(system.GetTrackedDeviceClass(41) == vr::TrackedDeviceClass_Invalid);

	slots.backend->devices.clear();
	CHECK(system.GetTrackedDeviceClass(40) == vr::TrackedDeviceClass_Controller);
	CHECK(system.GetTrackedDeviceClass(41) == vr::TrackedDeviceClass_Invalid);
	CHECK(system.GetTrackedDeviceClass(vr::k_unMaxTrackedDeviceCount) == vr::TrackedDeviceClass_Invalid);
}

TEST_CASE("Controller roles follow the connected devices")
{
	WithSlots slots;
	BaseSystem system;
	CHECK(system.GetControllerRoleForTrackedDeviceIndex(1) == vr::TrackedControllerRole_Invalid);
	CHECK(system.GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand) == vr::k_unTrackedDeviceIndexInvalid);

	slots.Add(5, vr::TrackedControllerRole_LeftHand, 0);
	slots.Add(2, vr::TrackedControllerRole_Invalid, 0)->cls = vr::TrackedDeviceClass_GenericTracker;
	CHECK(system.GetControllerRoleForTrackedDeviceIndex(5) == vr::TrackedControllerRole_LeftHand);
	CHECK(system.GetControllerRoleForTrackedDeviceIndex(2) == vr::TrackedControllerRole_Invalid);
	CHECK(system.GetControllerRoleForTrackedDeviceIndex(1) == vr::TrackedControllerRole_Invalid);
	CHECK(system.GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand) == 5);
	CHECK(system.GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand) == vr::k_unTrackedDeviceIndexInvalid);
	CHECK(system.GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_Invalid) == vr::k_unTrackedDeviceIndexInvalid);
}

TEST_CASE("Devices of a class are sorted right to left")
{
	WithSlots slots;
	BaseSystem system;
	slots.Add(1, vr::TrackedControllerRole_LeftHand, -0.3f);
	slots.Add(2, vr::TrackedControllerRole_RightHand, 0.3f);
	slots.Add(3, vr::TrackedControllerRole_Invalid, 0)->valid = false;
	slots.Add(4, vr::TrackedControllerRole_Invalid, 0.1f);

	vr::TrackedDeviceIndex_t out[8] = {};
	// Relative to no device: the standing space, with x to the right
	CHECK(system.GetSortedTrackedDeviceIndicesOfClass(vr::TrackedDeviceClass_Controller, out, 8, vr::k_unTrackedDeviceIndexInvalid) == 4);
	CHECK(out[0] == 2);
	CHECK(out[1] == 4);
	CHECK(out[2] == 1);
	CHECK(out[3] == 3);

	// Seen from the device at 0.1, whose axes are the same, the order is the same
	CHECK(system.GetSortedTrackedDeviceIndicesOfClass(vr::TrackedDeviceClass_Controller, out, 8, 4) == 4);
	CHECK(out[0] == 2);

	// A short array gets the first ones; the count is the whole list
	vr::TrackedDeviceIndex_t one[1] = {};
	CHECK(system.GetSortedTrackedDeviceIndicesOfClass(vr::TrackedDeviceClass_Controller, one, 1, vr::k_unTrackedDeviceIndexInvalid) == 4);
	CHECK(one[0] == 2);

	// No array: the count only
	CHECK(system.GetSortedTrackedDeviceIndicesOfClass(vr::TrackedDeviceClass_Controller, nullptr, 3, vr::k_unTrackedDeviceIndexInvalid) == 4);
	CHECK(system.GetSortedTrackedDeviceIndicesOfClass(vr::TrackedDeviceClass_HMD, nullptr, 0, vr::k_unTrackedDeviceIndexInvalid) == 0);
}

#include "Misc/xrmoreutils.h"
#include <cmath>

TEST_CASE("A seated origin placed at the head keeps the heading and drops pitch and roll")
{
	const float quarter = 3.14159265f / 4;
	// Turned a quarter to the left about the vertical axis, then pitched 30 degrees up
	glm::quat turn = glm::angleAxis(2 * quarter, glm::vec3(0, 1, 0));
	glm::quat pitch = glm::angleAxis(3.14159265f / 6, glm::vec3(1, 0, 0));
	glm::quat headRotation = turn * pitch;
	XrPosef head = { { headRotation.x, headRotation.y, headRotation.z, headRotation.w }, { 0.5f, 1.6f, -0.25f } };
	XrPosef identity = { { 0, 0, 0, 1 }, { 0, 0, 0 } };

	XrPosef placed = xr_utils::SeatedOriginAtHead(identity, head);
	CHECK(placed.position.x == doctest::Approx(0.5f));
	CHECK(placed.position.y == doctest::Approx(1.6f));
	CHECK(placed.position.z == doctest::Approx(-0.25f));
	CHECK(placed.orientation.x == doctest::Approx(0.0f).epsilon(1e-5));
	CHECK(placed.orientation.z == doctest::Approx(0.0f).epsilon(1e-5));
	CHECK(placed.orientation.y == doctest::Approx(std::sin(quarter)));
	CHECK(placed.orientation.w == doctest::Approx(std::cos(quarter)));

	// Placing again at the same head, which is now at the origin's centre, changes nothing
	XrPosef again = xr_utils::SeatedOriginAtHead(placed, { { 0, 0, 0, 1 }, { 0, 0, 0 } });
	CHECK(again.position.x == doctest::Approx(0.5f));
	CHECK(again.orientation.y == doctest::Approx(std::sin(quarter)));
}

TEST_CASE("Looking straight up has no heading")
{
	glm::quat up = glm::angleAxis(3.14159265f / 2, glm::vec3(1, 0, 0));
	XrQuaternionf yaw = xr_utils::YawOnly({ up.x, up.y, up.z, up.w });
	CHECK(yaw.y == 0.0f);
	CHECK(yaw.w == 1.0f);
}

TEST_CASE("Poses are located as far ahead as asked")
{
	const XrTime now = 1000000000, frame = 1016000000;
	// Without a clock the default time stays
	CHECK(xr_utils::PoseTimeForPrediction(0.01f, 0, frame) == 0);
	// Now, and ahead of now
	CHECK(xr_utils::PoseTimeForPrediction(0.0f, now, 0) == now);
	CHECK(xr_utils::PoseTimeForPrediction(0.25f, now, 0) == now + 250000000);
	CHECK(xr_utils::PoseTimeForPrediction(0.0f, now, frame) == now);
	// Close to the frame's display time is the frame's
	CHECK(xr_utils::PoseTimeForPrediction(0.015f, now, frame) == frame);
	// Out of range or not a number is held to the range
	CHECK(xr_utils::PoseTimeForPrediction(100.0f, now, 0) == now + 1000000000);
	CHECK(std::llabs(xr_utils::PoseTimeForPrediction(std::nanf(""), now, 0) - (now - 100000000)) < 10);
}

namespace {
std::vector<vr::VREvent_t> Drain(BaseSystem& system)
{
	std::vector<vr::VREvent_t> out;
	vr::VREvent_t e{};
	while (system.PollNextEvent(&e, sizeof(e)))
		out.push_back(e);
	return out;
}
}

TEST_CASE("The devices present at the first poll are activated")
{
	WithSlots slots;
	BaseSystem system;
	slots.Add(0, vr::TrackedControllerRole_Invalid, 0)->cls = vr::TrackedDeviceClass_HMD;
	slots.Add(2, vr::TrackedControllerRole_RightHand, 0);
	slots.Add(7, vr::TrackedControllerRole_Invalid, 0)->cls = vr::TrackedDeviceClass_GenericTracker;

	std::vector<vr::VREvent_t> first = Drain(system);
	REQUIRE(first.size() == 3);
	CHECK(first[0].eventType == vr::VREvent_TrackedDeviceActivated);
	CHECK(first[0].trackedDeviceIndex == 0);
	CHECK(first[1].trackedDeviceIndex == 2);
	CHECK(first[2].trackedDeviceIndex == 7);

	// A device that joins later is its backend's to announce
	slots.Add(3, vr::TrackedControllerRole_LeftHand, 0);
	CHECK(Drain(system).empty());
}

TEST_CASE("A device already announced is not announced again at the first poll")
{
	WithSlots slots;
	BaseSystem system;
	slots.Add(1, vr::TrackedControllerRole_LeftHand, 0);
	slots.Add(2, vr::TrackedControllerRole_RightHand, 0);
	vr::VREvent_t activated{};
	activated.eventType = vr::VREvent_TrackedDeviceActivated;
	activated.trackedDeviceIndex = 1;
	system._EnqueueEvent(activated);

	std::vector<vr::VREvent_t> events = Drain(system);
	REQUIRE(events.size() == 2);
	CHECK(events[0].trackedDeviceIndex == 1);
	CHECK(events[1].trackedDeviceIndex == 2);
}

TEST_CASE("The event queue keeps the newest events when nobody polls")
{
	WithSlots slots;
	BaseSystem system;
	Drain(system);
	for (uint32_t i = 0; i < 6000; i++) {
		vr::VREvent_t e{};
		e.eventType = vr::VREvent_ButtonPress;
		e.data.controller.button = i;
		system._EnqueueEvent(e);
	}
	std::vector<vr::VREvent_t> events = Drain(system);
	REQUIRE(events.size() == 4096);
	CHECK(events.front().data.controller.button == 6000 - 4096);
	CHECK(events.back().data.controller.button == 5999);
}

TEST_CASE("The application is told to pause only after it had input focus and lost it")
{
	WithSlots slots;
	BaseSystem system;
	// Before the first frame is shown (or ever focused): no pause, no controllers drawn over the game
	CHECK_FALSE(system.ShouldApplicationPause());
	CHECK_FALSE(system.IsSteamVRDrawingControllers());
	CHECK_FALSE(system.ShouldApplicationReduceRenderingWork());

	slots.backend->lost = true;
	CHECK(system.ShouldApplicationPause());
	CHECK(system.IsSteamVRDrawingControllers());
	CHECK_FALSE(system.ShouldApplicationReduceRenderingWork());
}

TEST_CASE("The application is told to reduce its rendering when the runtime is short of time")
{
	WithSlots slots;
	BaseSystem system;
	slots.backend->strained = true;
	CHECK(system.ShouldApplicationReduceRenderingWork());
	CHECK_FALSE(system.ShouldApplicationPause());
}
