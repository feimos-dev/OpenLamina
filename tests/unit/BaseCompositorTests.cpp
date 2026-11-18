// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

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
	// All zero (a mod that never fills them in): the whole texture, as before
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
