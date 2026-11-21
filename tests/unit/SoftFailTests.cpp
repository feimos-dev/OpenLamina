#include "doctest.h"

#include "stdafx.h"

#include "Misc/SoftFail.h"
#include "logging.h"

#include <atomic>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

std::mutex sink_lock;
std::vector<std::string> sink_lines;
void sink(const char* line)
{
	std::lock_guard<std::mutex> g(sink_lock);
	sink_lines.emplace_back(line);
}
size_t count_lines(const std::string& needle)
{
	std::lock_guard<std::mutex> g(sink_lock);
	size_t n = 0;
	for (const std::string& l : sink_lines)
		n += l.find(needle) != std::string::npos;
	return n;
}
struct Capture {
	Capture()
	{
		std::lock_guard<std::mutex> g(sink_lock);
		sink_lines.clear();
		oovr_log_set_test_sink(sink);
	}
	~Capture() { oovr_log_set_test_sink(nullptr); }
};

int unsupported_answer(int x)
{
	OC_UNSUPPORTED(42, "unsupported-answer-%d", x);
}

const OcSoftSite* site_named(const std::string& func)
{
	for (const OcSoftSite* s : oc_soft_sites())
		if (func == s->func)
			return s;
	return nullptr;
}

void counted_three_times()
{
	OC_UNSUPPORTED_VOID("counted-three-times");
}

void concurrent_hit()
{
	OC_UNSUPPORTED_VOID("concurrent-hit");
}

void legacy_soft_abort()
{
	OOVR_SOFT_ABORT("legacy-soft-abort");
}

std::atomic<int> hook_calls{ 0 };
void abort_hook(const char*) { hook_calls++; }

void stop_me()
{
	OC_UNSUPPORTED_VOID("stop-me");
}

} // namespace

TEST_CASE("SoftFail_first_hit_logs_once")
{
	Capture c;
	CHECK(unsupported_answer(1) == 42);
	CHECK(unsupported_answer(2) == 42);
	CHECK(count_lines("unsupported-answer-1") == 1);
	CHECK(count_lines("unsupported-answer-2") == 0);
}

TEST_CASE("SoftFail_counts_every_hit")
{
	for (int i = 0; i < 3; i++)
		counted_three_times();
	const OcSoftSite* s = site_named("counted_three_times");
	REQUIRE(s != nullptr);
	CHECK(s->hits.load() == 3);
}

TEST_CASE("SoftFail_concurrent_hits_count_exactly")
{
	Capture c;
	std::vector<std::thread> threads;
	for (int t = 0; t < 8; t++)
		threads.emplace_back([] {
			for (int i = 0; i < 10000; i++)
				concurrent_hit();
		});
	for (std::thread& t : threads)
		t.join();
	const OcSoftSite* s = site_named("concurrent_hit");
	REQUIRE(s != nullptr);
	CHECK(s->hits.load() == 80000);
	CHECK(count_lines("concurrent-hit") == 1);
}

TEST_CASE("SoftFail_stop_on_soft_abort_calls_abort_hook")
{
	oc_soft_set_abort_hook(abort_hook);
	oc_soft_set_stop_for_tests(1);
	stop_me();
	oc_soft_set_stop_for_tests(-1);
	oc_soft_set_abort_hook(nullptr);
	CHECK(hook_calls.load() == 1);
}

TEST_CASE("Summary_lists_sites_with_counts")
{
	counted_three_times();
	Capture c;
	oc_soft_log_summary();
	CHECK(count_lines("compatibility summary") == 1);
	CHECK(count_lines("counted_three_times") == 1);
}

TEST_CASE("Soft_abort_macro_counts")
{
	legacy_soft_abort();
	legacy_soft_abort();
	const OcSoftSite* s = site_named("legacy_soft_abort");
	REQUIRE(s != nullptr);
	CHECK(s->hits.load() == 2);
}

TEST_CASE("Strcpy_arr_cuts_a_name_that_does_not_fit")
{
	Capture c;
	char small[8];
	strcpy_arr(small, "fits");
	CHECK(std::string(small) == "fits");
	strcpy_arr(small, "1234567");
	CHECK(std::string(small) == "1234567");
	CHECK(count_lines("Cut to") == 0);
	for (int i = 0; i < 2; i++)
		strcpy_arr(small, "/actions/main/in/far_too_long");
	CHECK(std::string(small) == "/action");
	CHECK(count_lines("Cut to 7 bytes") == 1);
	strcpy_arr(small, "/actions/main/in/also_too_long");
	CHECK(count_lines("Cut to 7 bytes") == 2);
}

TEST_CASE("Strcpy_arr_cuts_at_a_character_boundary")
{
	char small[8];
	strcpy_arr(small, "ab\xC3\xA9\xC3\xA9\xC3\xA9"); // "abééé": the third é would end at byte 8
	CHECK(std::string(small) == "ab\xC3\xA9\xC3\xA9");
	strcpy_arr(small, "abcdef\xE2\x82\xAC"); // "abcdef€": the € starts at byte 6 and doesn't fit
	CHECK(std::string(small) == "abcdef");
}

TEST_CASE("Log_lines_from_several_threads_are_written_whole")
{
	constexpr int kThreads = 4;
	constexpr int kLines = 200;
	std::vector<std::thread> threads;
	for (int t = 0; t < kThreads; t++) {
		threads.emplace_back([t] {
			for (int i = 0; i < kLines; i++)
				OOVR_LOGF("whole-line-marker %d %d end", t, i);
		});
	}
	for (std::thread& th : threads)
		th.join();
	OOVR_LOG("whole-line-marker done");

	std::string folder = oovr_log_folder();
	std::string path = "openlamina.oc_unit_tests.log";
	if (!folder.empty())
		path = folder + "/" + path;
	std::ifstream in(path);
	REQUIRE(in.is_open());
	int whole = 0;
	std::string line;
	while (std::getline(in, line)) {
		if (line.find("whole-line-marker") == std::string::npos || line.find("done") != std::string::npos)
			continue;
		// "[time] function:line\t- whole-line-marker t i end", with nothing of another line inside it
		const bool ok = line.front() == '[' && line.find("]") != std::string::npos && line.find("\t- whole-line-marker ") != std::string::npos
		    && line.size() > 4 && line.compare(line.size() - 4, 4, " end") == 0 && line.find('[', 1) == std::string::npos;
		CHECK_MESSAGE(ok, line);
		whole += ok;
	}
	CHECK(whole == kThreads * kLines);
}

namespace {
int entry_that_throws(bool doThrow)
{
	try {
		if (doThrow)
			throw std::runtime_error("entry-that-throws");
		return 1;
	} OC_ENTRY_CATCH("entry_that_throws", 7)
}

void void_entry_that_throws()
{
	try {
		throw 5; // not a std::exception
	} OC_ENTRY_CATCH("void_entry_that_throws")
}
} // namespace

TEST_CASE("An_exception_ends_the_call_as_failed_and_is_logged_once")
{
	Capture c;
	CHECK(entry_that_throws(false) == 1);
	CHECK(entry_that_throws(true) == 7);
	CHECK(entry_that_throws(true) == 7);
	CHECK(count_lines("An exception reached entry_that_throws, which answers as failed: entry-that-throws") == 1);

	void_entry_that_throws();
	CHECK(count_lines("An exception reached void_entry_that_throws, which answers as failed: not a std::exception") == 1);
}
