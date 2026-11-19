// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

#include "Misc/Process.h"
#include "steamvr_abi.h"
#include "Reimpl/BaseOverlay.h"

#include <string>

using namespace vr;

// BaseOverlay links the client core, which calls the runtime's entry points; nothing here calls them
VR_INTERFACE void* VR_CALLTYPE VR_GetGenericInterface(const char*, vr::EVRInitError* error)
{
	if (error)
		*error = VRInitError_Init_InterfaceNotFound;
	return nullptr;
}
VR_INTERFACE const char* VR_CALLTYPE VR_GetVRInitErrorAsEnglishDescription(vr::EVRInitError)
{
	return "";
}
VR_INTERFACE const char* VR_CALLTYPE VR_GetVRInitErrorAsSymbol(vr::EVRInitError)
{
	return "";
}
VR_INTERFACE uint32_t VR_CALLTYPE VR_InitInternal2(vr::EVRInitError* error, vr::EVRApplicationType, const char*)
{
	if (error)
		*error = VRInitError_Init_HmdNotFound;
	return 0;
}
VR_INTERFACE bool VR_CALLTYPE VR_IsHmdPresent()
{
	return false;
}
VR_INTERFACE bool VR_CALLTYPE VR_IsInterfaceVersionValid(const char*)
{
	return false;
}
VR_INTERFACE void VR_CALLTYPE VR_ShutdownInternal() {}

namespace {

VROverlayHandle_t Make(BaseOverlay& overlays, const char* key)
{
	VROverlayHandle_t h = k_ulOverlayHandleInvalid;
	REQUIRE(overlays.CreateOverlay(key, "Test", &h) == VROverlayError_None);
	REQUIRE(h != k_ulOverlayHandleInvalid);
	return h;
}

} // namespace

TEST_CASE("A_new_overlay_is_white_opaque_and_unrestricted")
{
	BaseOverlay overlays;
	const VROverlayHandle_t h = Make(overlays, "oc.test.defaults");

	float r = -1, g = -1, b = -1, a = -1;
	CHECK(overlays.GetOverlayColor(h, &r, &g, &b) == VROverlayError_None);
	CHECK(overlays.GetOverlayAlpha(h, &a) == VROverlayError_None);
	CHECK(r == 1.0f);
	CHECK(g == 1.0f);
	CHECK(b == 1.0f);
	CHECK(a == 1.0f);

	float near = -1, far = -1;
	CHECK(overlays.GetOverlayAutoCurveDistanceRangeInMeters(h, &near, &far) == VROverlayError_None);
	CHECK(near == 0.0f);
	CHECK(far == 0.0f);

	// Its creator may render to it
	CHECK(overlays.GetOverlayRenderingPid(h) == oc_current_pid());

	// Hidden until shown
	CHECK_FALSE(overlays.IsOverlayVisible(h));
}

TEST_CASE("Overlay_getters_refuse_null_pointers")
{
	BaseOverlay overlays;
	const VROverlayHandle_t h = Make(overlays, "oc.test.null");
	float f = 0;
	CHECK(overlays.GetOverlayColor(h, nullptr, &f, &f) == VROverlayError_InvalidParameter);
	CHECK(overlays.GetOverlayAlpha(h, nullptr) == VROverlayError_InvalidParameter);
	CHECK(overlays.GetOverlayCurvature(h, nullptr) == VROverlayError_InvalidParameter);
	CHECK(overlays.GetOverlayAutoCurveDistanceRangeInMeters(h, &f, nullptr) == VROverlayError_InvalidParameter);
	CHECK(overlays.GetOverlayWidthInMeters(h, nullptr) == VROverlayError_InvalidParameter);
	CHECK(overlays.GetOverlaySortOrder(h, nullptr) == VROverlayError_InvalidParameter);
	CHECK(overlays.GetOverlayTextureColorSpace(h, nullptr) == VROverlayError_InvalidParameter);
	CHECK(overlays.GetOverlayTextureBounds(h, nullptr) == VROverlayError_InvalidParameter);
	CHECK(overlays.GetOverlayTransformType(h, nullptr) == VROverlayError_InvalidParameter);
	CHECK(overlays.GetOverlayFlag(h, (OOVR_VROverlayFlags)3, nullptr) == VROverlayError_InvalidParameter);
	CHECK(overlays.SetOverlayTransformAbsolute(h, TrackingUniverseStanding, nullptr) == VROverlayError_InvalidParameter);
	CHECK(overlays.GetOverlayTransformAbsolute(h, nullptr, nullptr) == VROverlayError_InvalidParameter);
	CHECK(overlays.SetOverlayTransformTrackedDeviceRelative(h, 0, nullptr) == VROverlayError_InvalidParameter);
}

TEST_CASE("An_absolute_transform_never_set_still_answers_an_origin")
{
	BaseOverlay overlays;
	const VROverlayHandle_t h = Make(overlays, "oc.test.origin");
	ETrackingUniverseOrigin origin = (ETrackingUniverseOrigin)99;
	HmdMatrix34_t m{};
	CHECK(overlays.GetOverlayTransformAbsolute(h, &origin, &m) == VROverlayError_None);
	CHECK((origin == TrackingUniverseSeated || origin == TrackingUniverseStanding));
}

TEST_CASE("Finding_an_overlay_that_does_not_exist_says_so")
{
	BaseOverlay overlays;
	VROverlayHandle_t h = 7;
	CHECK(overlays.FindOverlay("oc.test.nobody", &h) == VROverlayError_UnknownOverlay);
	CHECK(h == k_ulOverlayHandleInvalid);
}

TEST_CASE("At_most_128_overlays_exist")
{
	BaseOverlay overlays;
	VROverlayHandle_t h;
	for (int i = 0; i < 128; i++)
		CHECK(overlays.CreateOverlay(("oc.test.many." + std::to_string(i)).c_str(), "Test", &h) == VROverlayError_None);
	CHECK(overlays.CreateOverlay("oc.test.many.one.too.many", "Test", &h) == VROverlayError_OverlayLimitExceeded);
	CHECK(h == k_ulOverlayHandleInvalid);
	// A destroyed overlay makes room
	REQUIRE(overlays.FindOverlay("oc.test.many.0", &h) == VROverlayError_None);
	CHECK(overlays.DestroyOverlay(h) == VROverlayError_None);
	CHECK(overlays.CreateOverlay("oc.test.many.again", "Test", &h) == VROverlayError_None);
}

TEST_CASE("Overlay_relative_transform_is_only_read_back_when_set")
{
	BaseOverlay overlays;
	const VROverlayHandle_t child = Make(overlays, "oc.test.child");
	const VROverlayHandle_t parent = Make(overlays, "oc.test.parent");
	VROverlayHandle_t got = 5;
	HmdMatrix34_t m{};
	CHECK(overlays.GetOverlayTransformOverlayRelative(child, &got, &m) == VROverlayError_WrongTransformType);
	CHECK(overlays.SetOverlayTransformOverlayRelative(child, parent, &m) == VROverlayError_None);
	CHECK(overlays.GetOverlayTransformOverlayRelative(child, &got, &m) == VROverlayError_None);
	CHECK(got == parent);

	// Destroying the parent leaves nothing pointing at it
	CHECK(overlays.DestroyOverlay(parent) == VROverlayError_None);
	CHECK(overlays.GetOverlayTransformOverlayRelative(child, &got, &m) == VROverlayError_None);
	CHECK(got == k_ulOverlayHandleInvalid);
}

TEST_CASE("Any_overlay_event_is_polled_with_its_overlay_handle")
{
	BaseOverlay overlays;
	const VROverlayHandle_t first = Make(overlays, "oc.test.pollany.first");
	const VROverlayHandle_t second = Make(overlays, "oc.test.pollany.second");
	VREvent_t e{};
	VROverlayHandle_t from = k_ulOverlayHandleInvalid;
	CHECK_FALSE(overlays.PollNextAnyOverlayEvent(&e, sizeof(e), &from));

	REQUIRE(overlays.ShowOverlay(second) == VROverlayError_None);
	REQUIRE(overlays.PollNextAnyOverlayEvent(&e, sizeof(e), &from));
	CHECK(from == second);
	CHECK(e.eventType == VREvent_OverlayShown);
	CHECK_FALSE(overlays.PollNextAnyOverlayEvent(&e, sizeof(e), &from));
	(void)first;
}
