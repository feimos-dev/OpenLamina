// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

#include <limits>
#include <string>
#include <vector>

#include "Reimpl/BaseCompositor.h"

TEST_CASE("Vk_extensions_size_insufficient_answers_needed_size")
{
	// A buffer too small: the size needed, as SteamVR answers (a game asks again with a buffer that big)
	CHECK(BaseCompositor::VulkanExtensionsSize(XR_ERROR_SIZE_INSUFFICIENT, 87) == 87);
	// Filled, or only asked for its size: the size
	CHECK(BaseCompositor::VulkanExtensionsSize(XR_SUCCESS, 87) == 87);
	// Any other failure (no Vulkan in the runtime, say): 0
	CHECK(BaseCompositor::VulkanExtensionsSize(XR_ERROR_FUNCTION_UNSUPPORTED, 87) == 0);
	CHECK(BaseCompositor::VulkanExtensionsSize(XR_ERROR_VALIDATION_FAILURE, 87) == 0);
}

#include "Compositor/compositor.h"

TEST_CASE("Bounds_with_no_area_show_the_whole_texture_on_that_axis")
{
	TextureRegion r;
	// All zero (a mod that never fills them in): the whole texture
	vr::VRTextureBounds_t zero{ 0, 0, 0, 0 };
	REQUIRE(RegionFromBounds(&zero, 800, 600, r));
	CHECK(r.x0 == 0);
	CHECK(r.x1 == 800);
	CHECK(r.y0 == 0);
	CHECK(r.y1 == 600);
	CHECK_FALSE(r.flipX);
	CHECK_FALSE(r.flipY);

	// Only one axis without area: that axis is whole, the other kept (flipped as given)
	vr::VRTextureBounds_t half{ 0.5f, 1, 0.5f, 0 };
	REQUIRE(RegionFromBounds(&half, 800, 600, r));
	CHECK(r.x0 == 0);
	CHECK(r.x1 == 800);
	CHECK(r.y0 == 0);
	CHECK(r.y1 == 600);
	CHECK(r.flipY);

	// Normal bounds (uMin, vMin, uMax, vMax) are unchanged
	vr::VRTextureBounds_t left{ 0, 0.25f, 0.5f, 0.75f };
	REQUIRE(RegionFromBounds(&left, 800, 600, r));
	CHECK(r.x0 == 0);
	CHECK(r.x1 == 400);
	CHECK(r.y0 == 150);
	CHECK(r.y1 == 450);
	CHECK_FALSE(r.flipX);
	CHECK_FALSE(r.flipY);
}

TEST_CASE("Benchmark_rate_is_the_pixels_the_display_needs_per_second")
{
	// 2000x2200 per eye pair at 90 Hz
	CHECK(BaseCompositor::MegaPixelsPerSecond(4000, 2200, 90.0f) == doctest::Approx(792.0f));
	CHECK(BaseCompositor::MegaPixelsPerSecond(4000, 2200, 0.0f) == 0.0f);
}

TEST_CASE("A_frame_with_one_eye_shows_it_in_both")
{
	int source[2];
	const bool usable[2] = { true, true };

	const bool leftOnly[2] = { true, false };
	EyeImageSources(leftOnly, usable, source);
	CHECK(source[0] == 0);
	CHECK(source[1] == 0);

	const bool rightOnly[2] = { false, true };
	EyeImageSources(rightOnly, usable, source);
	CHECK(source[0] == 1);
	CHECK(source[1] == 1);

	const bool both[2] = { true, true };
	EyeImageSources(both, usable, source);
	CHECK(source[0] == 0);
	CHECK(source[1] == 1);

	// An eye submitted but unusable leaves the layer out; the one not submitted can't borrow it
	const bool leftBroken[2] = { false, true };
	EyeImageSources(both, leftBroken, source);
	CHECK(source[0] == -1);
	CHECK(source[1] == 1);
	EyeImageSources(leftOnly, leftBroken, source);
	CHECK(source[0] == -1);
	CHECK(source[1] == -1);

	// Nothing submitted: no image
	const bool none[2] = { false, false };
	EyeImageSources(none, usable, source);
	CHECK(source[0] == -1);
	CHECK(source[1] == -1);
}

TEST_CASE("Submit_ends_the_frame_unless_the_game_ends_it_with_PostPresentHandoff")
{
	CHECK(BaseCompositor::SubmitEndsFrame(VRCompositorTimingMode_Implicit, false));
	CHECK(BaseCompositor::SubmitEndsFrame(VRCompositorTimingMode_Explicit_RuntimePerformsPostPresentHandoff, false));
	CHECK_FALSE(BaseCompositor::SubmitEndsFrame(VRCompositorTimingMode_Explicit_ApplicationPerformsPostPresentHandoff, false));
	// A game that calls it ends its frames there in any mode
	CHECK_FALSE(BaseCompositor::SubmitEndsFrame(VRCompositorTimingMode_Implicit, true));
}

TEST_CASE("Submitted_bounds_with_a_NaN_are_not_bounds")
{
	const float nan = std::numeric_limits<float>::quiet_NaN();
	vr::VRTextureBounds_t good{ 0, 0, 1, 1 };
	CHECK(BaseCompositor::BoundsAreNumbers(nullptr));
	CHECK(BaseCompositor::BoundsAreNumbers(&good));
	for (int i = 0; i < 4; i++) {
		vr::VRTextureBounds_t bad = good;
		(&bad.uMin)[i] = nan;
		CHECK_FALSE(BaseCompositor::BoundsAreNumbers(&bad));
	}
}

TEST_CASE("The_raw_tracking_universe_is_answered_as_set")
{
	CHECK(BaseCompositor::UniverseAnswer(vr::TrackingUniverseRawAndUncalibrated, XR_REFERENCE_SPACE_TYPE_STAGE) == vr::TrackingUniverseRawAndUncalibrated);
	CHECK(BaseCompositor::UniverseAnswer(vr::TrackingUniverseStanding, XR_REFERENCE_SPACE_TYPE_STAGE) == vr::TrackingUniverseStanding);
	CHECK(BaseCompositor::UniverseAnswer(vr::TrackingUniverseSeated, XR_REFERENCE_SPACE_TYPE_LOCAL) == vr::TrackingUniverseSeated);
	// The space moved to seated by other means: seated
	CHECK(BaseCompositor::UniverseAnswer(vr::TrackingUniverseRawAndUncalibrated, XR_REFERENCE_SPACE_TYPE_LOCAL) == vr::TrackingUniverseSeated);
}

TEST_CASE("A_failed_texture_is_retried_at_once_unless_it_keeps_failing")
{
	int failures = 0;
	// The first failure of a texture: tried again at the next Submit; then further apart
	CHECK(RetryDelayAfterFailure(false, false, failures) == 0);
	CHECK(RetryDelayAfterFailure(false, true, failures) == 1);
	CHECK(RetryDelayAfterFailure(false, true, failures) == 3);
	// A wait that timed out is never backed off, and doesn't count
	CHECK(RetryDelayAfterFailure(true, true, failures) == 0);
	CHECK(RetryDelayAfterFailure(false, true, failures) == 7);
	// Another texture starts over
	CHECK(RetryDelayAfterFailure(false, false, failures) == 0);
	for (int i = 0; i < 20; i++)
		RetryDelayAfterFailure(false, true, failures);
	CHECK(RetryDelayAfterFailure(false, true, failures) == 90);
}

#if defined(SUPPORT_VK)
#include "tmp_gfx/TemporaryVk.h"

TEST_CASE("The_temporary_Vulkan_device_uses_a_queue_family_that_can_draw")
{
	VkQueueFamilyProperties families[3] = {};
	families[0].queueFlags = VK_QUEUE_TRANSFER_BIT;
	families[0].queueCount = 1;
	families[1].queueFlags = VK_QUEUE_COMPUTE_BIT;
	families[1].queueCount = 2;
	families[2].queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT;
	families[2].queueCount = 1;
	CHECK(PickGraphicsQueueFamily(families, 3) == 2);
	CHECK(PickGraphicsQueueFamily(families, 2) == -1);
	families[2].queueCount = 0;
	CHECK(PickGraphicsQueueFamily(families, 3) == -1);
}

TEST_CASE("A_Vulkan_loader_older_than_the_runtime_needs_is_refused")
{
	CHECK(VulkanVersionMeetsXr(VK_MAKE_VERSION(1, 1, 0), XR_MAKE_VERSION(1, 0, 0)));
	CHECK(VulkanVersionMeetsXr(VK_MAKE_VERSION(1, 3, 250), XR_MAKE_VERSION(1, 1, 0)));
	CHECK_FALSE(VulkanVersionMeetsXr(VK_MAKE_VERSION(1, 0, 65), XR_MAKE_VERSION(1, 1, 0)));
	CHECK(VulkanVersionMeetsXr(VK_MAKE_VERSION(2, 0, 0), XR_MAKE_VERSION(1, 3, 0)));
}
#endif

TEST_CASE("The_interop_extension_list_fills_the_buffer_it_fits_in")
{
	const char* list = BaseCompositor::InteropVulkanExtensions(false);
	const uint32_t needed = (uint32_t)strlen(list) + 1;
	CHECK(BaseCompositor::ExtensionListSize(list, nullptr, 0) == needed);
	std::vector<char> big(needed + 8, 'x');
	CHECK(BaseCompositor::ExtensionListSize(list, big.data(), (uint32_t)big.size()) == needed);
	CHECK(std::string(big.data()) == list);
	// Too small a buffer: what fits, ended, and the size to ask again with
	char small[10];
	CHECK(BaseCompositor::ExtensionListSize(list, small, sizeof(small)) == needed);
	CHECK(strlen(small) == 9);
	CHECK(strncmp(small, list, 9) == 0);
	CHECK(std::string(BaseCompositor::InteropVulkanExtensions(true)).find("VK_KHR_external_memory_win32") != std::string::npos);
}

#include "Reimpl/BaseScreenshots.h"

TEST_CASE("Screenshots_are_requested_of_a_game_that_hooked_them")
{
	BaseScreenshots screenshots;
	vr::ScreenshotHandle_t handle = 0;
	char name[64];
	BaseScreenshots::EVRScreenshotError error;
	using Error = BaseScreenshots::EVRScreenshotError;

	// Not hooked: nobody to take it
	CHECK(screenshots.RequestScreenshot(&handle, vr::VRScreenshotType_Stereo, "preview", "vr") == Error::VRScreenshotError_RequestFailed);
	CHECK(handle == vr::k_unScreenshotHandleInvalid);

	const vr::EVRScreenshotType types[] = { vr::VRScreenshotType_Stereo };
	CHECK(screenshots.HookScreenshot(types, 1) == Error::VRScreenshotError_None);
	// A type it didn't hook
	CHECK(screenshots.RequestScreenshot(&handle, vr::VRScreenshotType_Cubemap, "preview", "vr") == Error::VRScreenshotError_RequestFailed);

	REQUIRE(screenshots.RequestScreenshot(&handle, vr::VRScreenshotType_Stereo, "preview", "vr") == Error::VRScreenshotError_None);
	CHECK(handle != vr::k_unScreenshotHandleInvalid);
	CHECK(screenshots.GetScreenshotPropertyType(handle, &error) == vr::VRScreenshotType_Stereo);
	CHECK(error == Error::VRScreenshotError_None);
	CHECK(screenshots.GetScreenshotPropertyFilename(handle, vr::VRScreenshotPropertyFilenames_VR, name, sizeof(name), &error) == 3);
	CHECK(std::string(name) == "vr");
	char tiny[2];
	CHECK(screenshots.GetScreenshotPropertyFilename(handle, vr::VRScreenshotPropertyFilenames_Preview, tiny, sizeof(tiny), &error) == 8);
	CHECK(error == Error::VRScreenshotError_BufferTooSmall);

	// A handle nobody was given
	CHECK(screenshots.GetScreenshotPropertyType(handle + 100, &error) == vr::VRScreenshotType_None);
	CHECK(error == Error::VRScreenshotError_NotFound);
	CHECK(screenshots.GetScreenshotPropertyFilename(handle + 100, vr::VRScreenshotPropertyFilenames_VR, name, sizeof(name), &error) == 0);
	CHECK(error == Error::VRScreenshotError_NotFound);
	CHECK(std::string(name).empty());
}
