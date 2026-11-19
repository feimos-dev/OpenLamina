// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

#include "Reimpl/BasePaths.h"

TEST_CASE("Paths_keep_their_handles_up_to_a_limit")
{
	BasePaths p;
	OOVR_PathHandle_t first = 0, again = 0, h = 0;
	REQUIRE(p.StringToHandle(&first, "/user/head") == vr::TrackedProp_Success);
	for (size_t i = 1; i < BasePaths::kMaxPaths; i++)
		REQUIRE(p.StringToHandle(&h, ("/p/" + std::to_string(i)).c_str()) == vr::TrackedProp_Success);

	// Full: a new path gets none, a known one still answers
	h = 0;
	CHECK(p.StringToHandle(&h, "/one/too/many") == vr::TrackedProp_InvalidOperation);
	CHECK(p.StringToHandle(&again, "/user/head") == vr::TrackedProp_Success);
	CHECK(again == first);

	char buf[32];
	uint32_t used = 0;
	CHECK(p.HandleToString(first, buf, sizeof(buf), &used) == vr::TrackedProp_Success);
	CHECK(std::string(buf) == "/user/head");
}
