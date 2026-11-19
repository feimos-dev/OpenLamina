// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

#include "Reimpl/BaseMailbox.h"

#include <json/json.h>

TEST_CASE("Mailbox_ready_message_is_valid_json_and_comes_once")
{
	BaseMailbox mb;
	char buf[64];
	uint32_t len = 0;
	REQUIRE(mb.ReadMessage(1, buf, sizeof(buf), &len) == BaseMailbox::VR_MBox_None);
	CHECK(len == strlen(buf));

	Json::Value v;
	Json::CharReaderBuilder rb;
	std::string errs;
	std::istringstream in(buf);
	REQUIRE(Json::parseFromStream(rb, in, &v, &errs));
	CHECK(v["type"].asString() == "ready");

	CHECK(mb.ReadMessage(1, buf, sizeof(buf), &len) == BaseMailbox::VR_MBox_NoMessage);
}

TEST_CASE("Mailbox_read_survives_missing_pointers_and_short_buffers")
{
	BaseMailbox mb;
	// No length out-parameter, no buffer
	CHECK(mb.ReadMessage(1, nullptr, 0, nullptr) == BaseMailbox::VR_MBox_BufferTooShort);

	// A buffer one byte too short: the length says what to allocate, and the message is not lost
	uint32_t len = 0;
	char buf[64];
	CHECK(mb.ReadMessage(1, buf, 0, &len) == BaseMailbox::VR_MBox_BufferTooShort);
	REQUIRE(len > 0);
	memset(buf, 'x', sizeof(buf));
	CHECK(mb.ReadMessage(1, buf, len, &len) == BaseMailbox::VR_MBox_BufferTooShort);
	CHECK(buf[0] == 'x');
	CHECK(mb.ReadMessage(1, buf, len + 1, &len) == BaseMailbox::VR_MBox_None);
}

TEST_CASE("Mailbox_each_instance_sends_ready")
{
	char buf[64];
	uint32_t len = 0;
	{
		BaseMailbox mb;
		CHECK(mb.ReadMessage(1, buf, sizeof(buf), &len) == BaseMailbox::VR_MBox_None);
	}
	BaseMailbox again;
	CHECK(again.ReadMessage(1, buf, sizeof(buf), &len) == BaseMailbox::VR_MBox_None);
}
