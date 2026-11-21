// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

#include "Reimpl/BaseMailbox.h"
#include "generated/static_bases.gen.h"

#include <json/json.h>

#include <atomic>
#include <thread>
#include <vector>

TEST_CASE("Mailbox_ready_message_is_valid_json_and_comes_once")
{
	BaseMailbox mb;
	OOVR_mbox_handle h = 0;
	REQUIRE(mb.RegisterMailbox("game", &h) == BaseMailbox::VR_MBox_None);
	char buf[64];
	uint32_t len = 0;
	REQUIRE(mb.ReadMessage(h, buf, sizeof(buf), &len) == BaseMailbox::VR_MBox_None);
	CHECK(len == strlen(buf));

	Json::Value v;
	Json::CharReaderBuilder rb;
	std::string errs;
	std::istringstream in(buf);
	REQUIRE(Json::parseFromStream(rb, in, &v, &errs));
	CHECK(v["type"].asString() == "ready");

	CHECK(mb.ReadMessage(h, buf, sizeof(buf), &len) == BaseMailbox::VR_MBox_NoMessage);
}

TEST_CASE("Mailbox_read_survives_missing_pointers_and_short_buffers")
{
	BaseMailbox mb;
	OOVR_mbox_handle h = 0;
	REQUIRE(mb.RegisterMailbox("game", &h) == BaseMailbox::VR_MBox_None);
	// No length out-parameter: refused, as SteamVR does
	CHECK(mb.ReadMessage(h, nullptr, 0, nullptr) == BaseMailbox::VR_MBox_InvalidParameter);

	// No buffer, or one byte too short: the length says what to allocate, and the message is not lost
	uint32_t len = 0;
	char buf[64];
	CHECK(mb.ReadMessage(h, nullptr, 0, &len) == BaseMailbox::VR_MBox_BufferTooShort);
	CHECK(mb.ReadMessage(h, buf, 0, &len) == BaseMailbox::VR_MBox_BufferTooShort);
	REQUIRE(len > 0);
	memset(buf, 'x', sizeof(buf));
	CHECK(mb.ReadMessage(h, buf, len, &len) == BaseMailbox::VR_MBox_BufferTooShort);
	CHECK(buf[0] == 'x');
	CHECK(mb.ReadMessage(h, buf, len + 1, &len) == BaseMailbox::VR_MBox_None);
}

TEST_CASE("Mailbox_each_instance_sends_ready")
{
	char buf[64];
	uint32_t len = 0;
	OOVR_mbox_handle h = 0;
	{
		BaseMailbox mb;
		REQUIRE(mb.RegisterMailbox("game", &h) == BaseMailbox::VR_MBox_None);
		CHECK(mb.ReadMessage(h, buf, sizeof(buf), &len) == BaseMailbox::VR_MBox_None);
	}
	BaseMailbox again;
	REQUIRE(again.RegisterMailbox("game", &h) == BaseMailbox::VR_MBox_None);
	CHECK(again.ReadMessage(h, buf, sizeof(buf), &len) == BaseMailbox::VR_MBox_None);
}

TEST_CASE("Mailbox_answers_as_SteamVR_does_for_mailboxes_it_doesnt_have")
{
	BaseMailbox mb;
	char buf[64];
	uint32_t len = 0;
	// Not registered: a small number made up by the caller isn't a mailbox
	CHECK(mb.ReadMessage(1, buf, sizeof(buf), &len) == BaseMailbox::VR_MBox_InvalidHandle);
	CHECK(mb.UnregisterMailbox(1) == BaseMailbox::VR_MBox_InvalidHandle);
	OOVR_mbox_handle h = 0;
	CHECK(mb.RegisterMailbox("", &h) == BaseMailbox::VR_MBox_InvalidParameter);
	REQUIRE(mb.RegisterMailbox("game", &h) == BaseMailbox::VR_MBox_None);
	CHECK(h != 1);
	CHECK(mb.UnregisterMailbox(h) == BaseMailbox::VR_MBox_None);
	CHECK(mb.UnregisterMailbox(h) == BaseMailbox::VR_MBox_InvalidHandle);
	// A message needs an address and a body; to any mailbox it's sent on
	CHECK(mb.SendMessage(h, "", "") == BaseMailbox::VR_MBox_InvalidParameter);
	CHECK(mb.SendMessage(0, nullptr, nullptr) == BaseMailbox::VR_MBox_InvalidParameter);
	CHECK(mb.SendMessage(1, "test", "test") == BaseMailbox::VR_MBox_None);
}

TEST_CASE("Mailbox_names_have_their_own_handles")
{
	BaseMailbox mb;
	OOVR_mbox_handle a = 0, b = 0, again = 0;
	REQUIRE(mb.RegisterMailbox("one", &a) == BaseMailbox::VR_MBox_None);
	REQUIRE(mb.RegisterMailbox("two", &b) == BaseMailbox::VR_MBox_None);
	REQUIRE(mb.RegisterMailbox("one", &again) == BaseMailbox::VR_MBox_None);
	CHECK(a != 0);
	CHECK(b != 0);
	CHECK(a != b);
	CHECK(again == a);

	// A name that was unregistered gets a handle of its own again
	CHECK(mb.UnregisterMailbox(a) == BaseMailbox::VR_MBox_None);
	REQUIRE(mb.RegisterMailbox("one", &again) == BaseMailbox::VR_MBox_None);
	CHECK(again != a);
	CHECK(again != b);
}

TEST_CASE("Mailbox_register_with_missing_pointers")
{
	BaseMailbox mb;
	OOVR_mbox_handle h = 5;
	CHECK(mb.RegisterMailbox(nullptr, &h) == BaseMailbox::VR_MBox_InvalidParameter);
	CHECK(h == 0);
	CHECK(mb.RegisterMailbox("x", nullptr) == BaseMailbox::VR_MBox_InvalidParameter);
}

TEST_CASE("Base_object_made_from_several_threads_at_once_is_one_object")
{
	constexpr int kThreads = 8;
	std::vector<std::shared_ptr<BaseMailbox>> made(kThreads);
	std::atomic<int> waiting{ kThreads };
	std::vector<std::thread> threads;
	for (int i = 0; i < kThreads; i++) {
		threads.emplace_back([&, i] {
			waiting--;
			// All start together, but none waits long if one is slow to start
			for (int spin = 0; spin < 100000 && waiting.load() > 0; spin++)
				std::this_thread::yield();
			made[i] = GetCreateBaseMailbox();
		});
	}
	for (std::thread& t : threads)
		t.join();

	REQUIRE(made[0]);
	for (const auto& m : made)
		CHECK(m == made[0]);
	CHECK(GetUnsafeBaseMailbox() == made[0].get());
	CHECK(GetBaseMailbox() == made[0]);

	made.clear();
	CHECK(GetBaseMailbox() == nullptr);
	CHECK(GetUnsafeBaseMailbox() == nullptr);
}
