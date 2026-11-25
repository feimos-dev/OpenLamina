#include "stdafx.h"

#include "Misc/backtrace.h"
#include "Misc/Config.h"
#include "logging.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <errno.h> // errno, ENOENT, EEXIST
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdarg.h>
#include <sys/stat.h> // stat
#ifdef _WIN32
#include <direct.h> // _mkdir
#endif
#ifndef _WIN32
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
extern char** environ;
#endif

// strftime format
#define LOGGER_TIME_FORMAT "%Y-%m-%d %H:%M:%S"

// printf format
#define LOGGER_MS_FORMAT ".%03d"

// convert current time to milliseconds since unix epoch
template <typename T>
static int get_ms(const std::chrono::time_point<T>& tp)
{
	using namespace std::chrono;

	auto dur = tp.time_since_epoch();
	auto s = std::chrono::duration_cast<std::chrono::seconds>(dur);
	std::chrono::duration<long, std::milli> rounded_ms = s;
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(dur);
	return static_cast<int>((ms - rounded_ms).count());
}

// format it in two parts: main part with date and time and part with milliseconds
static std::string format_time()
{
	auto tp = std::chrono::system_clock::now();
	std::time_t current_time = std::chrono::system_clock::to_time_t(tp);

	// Each thread its own tm: std::localtime's is shared
	std::tm time_info{};
#ifdef _WIN32
	localtime_s(&time_info, &current_time);
#else
	localtime_r(&current_time, &time_info);
#endif

	char buffer[128];

	size_t string_size = strftime(
	    buffer, sizeof(buffer),
	    LOGGER_TIME_FORMAT,
	    &time_info);

	int ms = get_ms(tp);

	string_size += std::snprintf(
	    buffer + string_size, sizeof(buffer) - string_size,
	    LOGGER_MS_FORMAT, ms);

	return std::string(buffer, buffer + string_size);
}

bool isDirExist(const std::string& path)
{
#if defined(_WIN32)
	struct _stat info;
	if (_stat(path.c_str(), &info) != 0) {
		return false;
	}
	return (info.st_mode & _S_IFDIR) != 0;
#else
	struct stat info;
	if (stat(path.c_str(), &info) != 0) {
		return false;
	}
	return (info.st_mode & S_IFDIR) != 0;
#endif
}

bool makePath(const std::string& path)
{
#if defined(_WIN32)
	int ret = _mkdir(path.c_str());
#else
	mode_t mode = 0755;
	int ret = mkdir(path.c_str(), mode);
#endif
	if (ret == 0)
		return true;

	switch (errno) {
	case ENOENT:
		// parent didn't exist, try to create it
		{
			size_t pos = path.find_last_of('/');
			if (pos == std::string::npos)
#if defined(_WIN32)
				pos = path.find_last_of('\\');
			if (pos == std::string::npos)
#endif
				return false;
			if (!makePath(path.substr(0, pos)))
				return false;
		}
		// now, try to create again
#if defined(_WIN32)
		return 0 == _mkdir(path.c_str());
#else
		return 0 == mkdir(path.c_str(), mode);
#endif

	case EEXIST:
		// done!
		return isDirExist(path);

	default:
		return false;
	}
}

std::string GetEnv(const std::string& var)
{
	const char* val = std::getenv(var.c_str());
	if (val == nullptr) {
		return "";
	} else {
		return val;
	}
}

#ifdef ANDROID
#include <android/log.h>
#else

#ifdef __GLIBCXX__
#include <ext/stdio_filebuf.h>

// We need stream_fd to be thread-safe and signal-safe.
#if __cpp_lib_atomic_lock_free_type_aliases >= 201907L
	static std::atomic_signed_lock_free stream_fd = -1;
#else
	static std::atomic_int stream_fd = -1;
#endif

typedef std::basic_ofstream<char>::__filebuf_type buffer_t;
typedef __gnu_cxx::stdio_filebuf<char> io_buffer_t;
static FILE* cfile_impl(buffer_t* fb)
{
	return (static_cast<io_buffer_t*>(fb))->file();
}

static FILE* cfile(std::ofstream& ofs)
{
	return cfile_impl(ofs.rdbuf());
}
#endif

// The program's executable name without its folder and extension, in characters safe for a file name; "program" if
// it can't be told
static std::string ProgramName()
{
	std::string name;
#ifdef _WIN32
	char path[MAX_PATH] = {};
	const DWORD length = GetModuleFileNameA(nullptr, path, sizeof(path));
	if (length > 0 && length < sizeof(path))
		name.assign(path, length);
#else
	char path[4096] = {};
	const ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 1);
	if (length > 0)
		name.assign(path, (size_t)length);
#endif
	const size_t slash = name.find_last_of("/\\");
	if (slash != std::string::npos)
		name.erase(0, slash + 1);
	const size_t dot = name.rfind('.');
	if (dot != std::string::npos && dot > 0)
		name.erase(dot);
	for (char& c : name) {
		const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
		if (!safe)
			c = '_';
	}
	return name.empty() ? "program" : name;
}

std::string oovr_log_folder()
{
	// The standard location; OC_LOG_DIR (used by the tests) replaces it
	string overrideFolder = GetEnv("OC_LOG_DIR");
#ifdef _WIN32
	string outputFolder = GetEnv("LOCALAPPDATA");
	if (!outputFolder.empty())
		outputFolder = outputFolder + "\\OpenLamina\\logs";
#else
	string outputFolder = GetEnv("XDG_STATE_HOME");
	if (outputFolder.empty()) {
		outputFolder = GetEnv("HOME");
		if (!outputFolder.empty())
			outputFolder = outputFolder + "/.local/state";
	}
	if (!outputFolder.empty())
		outputFolder = outputFolder + "/OpenLamina/logs";
#endif
	if (!overrideFolder.empty())
		outputFolder = overrideFolder;
	if (!outputFolder.empty() && makePath(outputFolder))
		return outputFolder;
	return {};
}

static std::ofstream init_stream()
{
	std::ofstream stream;
	// One file per program, by its executable's name: a game and the tools beside it each keep their own
	string outputFilePath = "openlamina." + ProgramName() + ".log";

	// Try and write to standard location
	// fall back to exe dir if can't create dir
	const string outputFolder = oovr_log_folder();
#ifdef _WIN32
	if (!outputFolder.empty())
		outputFilePath = outputFolder + "\\" + outputFilePath;
#else
	if (!outputFolder.empty())
		outputFilePath = outputFolder + "/" + outputFilePath;
#endif

	stream.open(outputFilePath.c_str());
#ifdef __GLIBCXX__
	if (stream.is_open())
		stream_fd.store(fileno(cfile(stream)), std::memory_order::seq_cst);
#endif
	return stream;
}

static std::ofstream& stream()
{
	struct LogStream {
		std::ofstream stream;
		~LogStream()
		{
#ifdef __GLIBCXX__
			// The signal-safe writers mustn't use the fd once it's closed
			stream_fd.store(-1, std::memory_order::seq_cst);
#endif
		}
	};
	static LogStream res{ init_stream() };
	return res.stream;
}

#endif

void oovr_printf_safe(const char* format, ...)
{
#ifdef __GLIBCXX__
	int fd = stream_fd.load(std::memory_order::seq_cst);
	if (fd < 0) {
		return;
	}

	int r;
	char buf[2048];
	va_list args;
	va_start(args, format);
	r = vsnprintf(buf, sizeof(buf), format, args);
	va_end(args);
	if (r < 0) {
		return;
	}
	// vsnprintf answers the length it would have written, which can be more than the buffer holds
	if (r >= (int)sizeof(buf))
		r = (int)sizeof(buf) - 1;

	size_t len = r;
	size_t offset = 0;
	while (len > 0) {
		r = write(fd, &buf[offset], len);
		if (r < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}

		offset += r;
		len -= r;
	}
#endif
}

void oovr_flush_safe()
{
#ifdef __GLIBCXX__
	int fd = stream_fd.load(std::memory_order::seq_cst);
	if (fd < 0) {
		return;
	}

	int r;
	do {
		r = fsync(fd);
	} while (r < 0 && errno == EINTR);
#endif
}

OC_NORETURN void oovr_abort_raw_va(const char* file, long line, const char* func, const char* msg, const char* title, va_list args);

static std::atomic<void (*)(const char*)> log_test_sink{ nullptr };

// Lines come from any thread; one is written whole before the next. Never destroyed, as threads may log while the
// process exits.
static std::mutex& log_mutex()
{
	static std::mutex* mutex = new std::mutex();
	return *mutex;
}

void oovr_log_set_test_sink(void (*sink)(const char* line))
{
	log_test_sink.store(sink);
}

// OC_NO_MESSAGE_BOX=1 (used by the tests): no message box, and a stop ends the process with exit code 70
static bool no_message_box()
{
	static const bool value = GetEnv("OC_NO_MESSAGE_BOX") == "1";
	return value;
}

void oovr_log_raw(const char* file, long line, const char* func, const char* msg)
{
	if (auto sink = log_test_sink.load()) {
		std::string text = std::string(func ? func : "") + ":" + std::to_string(line) + "\t- " + (msg ? msg : "NULL");
		sink(text.c_str());
	}

#ifdef ANDROID
	__android_log_print(ANDROID_LOG_INFO, "OpenLamina", "%s:%d \t %s", func, line, msg);
#else
	std::string text = "[" + format_time() + "] " + (func ? func : "") + ":" + std::to_string(line) + "\t- " + (msg ? msg : "NULL") + "\n";
	{
		std::lock_guard<std::mutex> lock(log_mutex());
		std::ofstream& out = stream();
		out.write(text.data(), (std::streamsize)text.size());
		out.flush();
	}

	// Write it to stdout
	// TODO on Windows, write it into the debug log
#ifndef _WIN32
	printf("[OC] %s:%ld \t %s\n", func, line, msg);
#endif

#endif
}

void oovr_log_raw_format(const char* file, long line, const char* func, const char* msg, ...)
{
	va_list args;
	va_start(args, msg);

	char buff[2048];
	vsnprintf(buff, sizeof(buff), msg, args);

	oovr_log_raw(file, line, func, buff);

	va_end(args);
}

void oc_strcpy_cut(char* dest, size_t dest_size, const char* src, std::atomic<bool>* logged, const char* file, long line)
{
	if (!dest || dest_size == 0)
		return;
	if (!src)
		src = "";
	size_t n = strnlen(src, dest_size);
	if (n == dest_size) {
		n = dest_size - 1;
		// Not in the middle of a UTF-8 character: src[n], the first byte left out, isn't a continuation byte
		while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80)
			n--;
		if (logged && !logged->exchange(true))
			oovr_log_raw_format(file, line, "strcpy_arr", "Cut to %d bytes, as it doesn't fit: %s (logged once here)", (int)n, src);
	}
	memcpy(dest, src, n);
	dest[n] = 0;
}

OC_NORETURN void oovr_abort_raw(const char* file, long line, const char* func, const char* msg, const char* title, ...)
{
	va_list args;
	va_start(args, title);
	oovr_abort_raw_va(file, line, func, msg, title, args);
	va_end(args); // Well we probably don't need this, but it should keep any tools happy
}

OC_NORETURN void oovr_abort_raw_va(const char* file, long line, const char* func, const char* msg, const char* title, va_list args)
{
	if (title == nullptr) {
		title = "OpenLamina Error - info in log";
		OOVR_LOG("Abort!");
	} else {
		OOVR_LOG(title);
	}

	char buff[2048];
	vsnprintf(buff, sizeof(buff), msg, args);
	buff[sizeof(buff) - 1] = 0;

	oovr_log_raw(file, line, func, buff);

	// Ensure everything gets written
#ifdef ANDROID
	__android_log_print(ANDROID_LOG_ERROR, "OpenLamina", "ERROR: %s:%d \t %s", func, line, buff);
#else
	{
		std::lock_guard<std::mutex> lock(log_mutex());
		stream() << std::flush;
	}
#endif

	if (no_message_box()) {
		std::_Exit(70);
	}

	OOVR_MESSAGE(buff, title);
	DebugBreak();
	oovr_dump_backtrace();
	abort();
}

void oovr_message_raw(const char* message, const char* title)
{
	// No need to log this, it will have already been done by the caller
	if (no_message_box()) {
		return;
	}

#ifdef WIN32
	// Display a message box on Windows
	MessageBoxA(nullptr, message, title, MB_OK);
#else
	// zenity or kdialog on Linux, started with the title and message as arguments (no shell reads them), otherwise
	// just print to stderr. zenity's --text is markup, so --no-markup keeps it as text.
	const std::string titleText = title ? title : "", messageText = message ? message : "";
	const std::vector<std::vector<std::string>> dialogs = {
		{ "zenity", "--error", "--no-markup", "--title", titleText, "--text", messageText },
		{ "kdialog", "--title", titleText, "--error", messageText },
	};
	bool message_displayed = false;
	for (const std::vector<std::string>& dialog : dialogs) {
		std::vector<char*> argv;
		for (const std::string& a : dialog)
			argv.push_back(const_cast<char*>(a.c_str()));
		argv.push_back(nullptr);
		pid_t pid = 0;
		// Not found (or not startable): the next one
		if (posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ) != 0)
			continue;
		int status = 0;
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
		}
		message_displayed = WIFEXITED(status) && WEXITSTATUS(status) == 0;
		break;
	}

	if (!message_displayed) {
		std::cerr << "OOVR_MESSAGE: " << titleText << ": " << messageText << std::endl;
	}
#endif
}
