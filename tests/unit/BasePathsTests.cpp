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

TEST_CASE("Paths_answer_as_SteamVR")
{
	BasePaths p;
	OOVR_PathHandle_t h = 0;
	CHECK(p.StringToHandle(&h, "") == vr::TrackedProp_InvalidOperation);
	REQUIRE(p.StringToHandle(&h, "/user/head") == vr::TrackedProp_Success);
	CHECK(h == BasePaths::kFirstHandle);
	char buf[32];
	uint32_t used = 0;
	CHECK(p.HandleToString(1, buf, sizeof(buf), &used) == vr::TrackedProp_InvalidOperation);

	// A batch names a property container there isn't; each entry says so too
	struct Read {
		vr::PropertyTypeTag_t unTag = 7;
		uint32_t unRequiredBufferSize = 7;
		vr::ETrackedPropertyError eError = vr::TrackedProp_Success;
	} reads[2];
	struct Write {
		vr::ETrackedPropertyError eError = vr::TrackedProp_Success;
	} writes[2];
	CHECK(p.ReadPathBatch(0, reads, 0) == vr::TrackedProp_Success);
	CHECK(p.ReadPathBatch(0, reads, 2) == vr::TrackedProp_InvalidDevice);
	CHECK(reads[1].eError == vr::TrackedProp_UnknownProperty);
	CHECK(reads[1].unTag == vr::k_unInvalidPropertyTag);
	CHECK(reads[1].unRequiredBufferSize == 0);
	CHECK(p.WritePathBatch(0, writes, 2) == vr::TrackedProp_InvalidDevice);
	CHECK(writes[1].eError == vr::TrackedProp_UnknownProperty);
	CHECK(p.ReadPathBatch(0, (Read*)nullptr, 1) == vr::TrackedProp_InvalidOperation);
}
