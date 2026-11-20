#include "stdafx.h"

#include "SharedStore.h"

#include "Misc/Process.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

constexpr size_t kStateSize = 16 * 1024;
constexpr size_t kHeaderSize = 128;
constexpr const char* kDocNames[] = { "settings", "applications", "chaperone", "scene", "dashboard", "overlays", "offsets" };

// The documents the user would lose work in (settings, registered applications, the room): written through to the
// disk with a last good copy beside them. The others describe running programs, change often (some from a game's
// frame), and are rebuilt as those programs run.
bool Kept(StoreDoc doc)
{
	return doc == StoreDoc::Settings || doc == StoreDoc::Applications || doc == StoreDoc::Chaperone || doc == StoreDoc::Offsets;
}

uint64_t Load(uint64_t& v)
{
	return std::atomic_ref<uint64_t>(v).load(std::memory_order_acquire);
}

void Store(uint64_t& v, uint64_t value)
{
	std::atomic_ref<uint64_t>(v).store(value, std::memory_order_release);
}

// How long a read or replace is tried again: well under the 2 s another program waits for the lock (Windows' sleeps
// round up to its 15.6 ms timer, so this is a time, not a count)
constexpr auto kRetryFor = std::chrono::milliseconds(500);

enum class ReadResult { Ok, Missing, Failed };

// The whole file, opened so that another process may replace it meanwhile. Missing if there's none; Failed if it's there
// but can't be read now (another program's antivirus or backup holding it, say), after trying again up to attempts
// times, 5 ms apart, and for kRetryFor at most.
ReadResult ReadShared(const fs::path& path, std::string& out, int attempts)
{
	auto deadline = std::chrono::steady_clock::now() + kRetryFor;
	for (int attempt = 0;; attempt++) {
		out.clear();
#ifdef _WIN32
		HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE) {
			DWORD err = GetLastError();
			if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
				return ReadResult::Missing;
		} else {
			char buffer[16384];
			DWORD got = 0;
			bool ok = true;
			while (true) {
				if (!ReadFile(h, buffer, sizeof(buffer), &got, nullptr)) {
					ok = false;
					break;
				}
				if (got == 0)
					break;
				out.append(buffer, got);
			}
			CloseHandle(h);
			if (ok)
				return ReadResult::Ok;
		}
#else
		int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
		if (fd < 0) {
			if (errno == ENOENT || errno == ENOTDIR)
				return ReadResult::Missing;
		} else {
			char buffer[16384];
			ssize_t got;
			while ((got = read(fd, buffer, sizeof(buffer))) > 0 || (got < 0 && errno == EINTR)) {
				if (got > 0)
					out.append(buffer, (size_t)got);
			}
			close(fd);
			if (got == 0)
				return ReadResult::Ok;
		}
#endif
		if (attempt + 1 >= attempts || std::chrono::steady_clock::now() >= deadline)
			return ReadResult::Failed;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
}

// The document's root object, or false if it doesn't parse (jsoncpp throws past its nesting limit)
bool ParseDoc(const std::string& text, Json::Value& root)
{
	Json::CharReaderBuilder builder;
	std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
	std::string error;
	try {
		return reader->parse(text.data(), text.data() + text.size(), &root, &error) && root.isObject();
	} catch (const std::exception&) {
		return false;
	}
}

// Moves from over to, replacing it. On Windows that fails (access denied, or a sharing violation) while another handle
// has the file open: a reader in some program, an antivirus or an indexer. None holds it long: tried again for
// kRetryFor.
bool Replace(const fs::path& from, const fs::path& to)
{
#ifdef _WIN32
	auto deadline = std::chrono::steady_clock::now() + kRetryFor;
	for (;;) {
		if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING))
			return true;
		DWORD err = GetLastError();
		if ((err != ERROR_ACCESS_DENIED && err != ERROR_SHARING_VIOLATION) || std::chrono::steady_clock::now() >= deadline)
			return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
#else
	return rename(from.c_str(), to.c_str()) == 0;
#endif
}

// The largest document written: more is a runaway, not a setting
constexpr size_t kMaxDocBytes = 16 * 1024 * 1024;

// Writes the file, and with durable asks the disk to have it before returning, so that a crash or power loss after
// the rename that follows doesn't leave a truncated document
bool WriteFile(const fs::path& path, const std::string& text, bool durable)
{
#ifdef _WIN32
	FILE* f = _wfopen(path.c_str(), L"wb");
#else
	FILE* f = fopen(path.c_str(), "wb");
#endif
	if (!f)
		return false;
	bool ok = fwrite(text.data(), 1, text.size(), f) == text.size() && fflush(f) == 0;
	if (ok && durable) {
#ifdef _WIN32
		ok = _commit(_fileno(f)) == 0;
#else
		ok = fsync(fileno(f)) == 0;
#endif
	}
	return fclose(f) == 0 && ok;
}

// The store folder of the program called folderName (OpenLamina, or OpenComposite before it)
fs::path StoreDirOf(const char* folderName)
{
#ifdef _WIN32
	if (const wchar_t* local = _wgetenv(L"LOCALAPPDATA"); local && local[0])
		return fs::path(local) / folderName / "store";
	return {};
#else
	const char* xdg = getenv("XDG_STATE_HOME");
	if (xdg && xdg[0])
		return fs::path(xdg) / folderName / "store";
	const char* home = getenv("HOME");
	if (home && home[0])
		return fs::path(home) / ".local" / "state" / folderName / "store";
	return {};
#endif
}

// OC_STORE_DIR, if it's set (tests)
fs::path OverrideDir()
{
#ifdef _WIN32
	if (const wchar_t* overrideDir = _wgetenv(L"OC_STORE_DIR"); overrideDir && overrideDir[0])
		return fs::path(overrideDir);
#else
	if (const char* overrideDir = getenv("OC_STORE_DIR"); overrideDir && overrideDir[0])
		return fs::path(overrideDir);
#endif
	return {};
}

std::string Utf8(const fs::path& p)
{
	std::u8string name = p.u8string();
	return std::string(name.begin(), name.end());
}

// Where this process's store is: OC_STORE_DIR if set, else the OpenLamina folder. If that doesn't exist yet and the
// OpenComposite one this was forked from does, its documents are copied over once (the old folder is left as it is,
// for an OpenComposite still installed); if they can't be, the old folder is used where it is.
fs::path ChooseDir()
{
	if (fs::path overridden = OverrideDir(); !overridden.empty())
		return overridden;
	fs::path dir = StoreDirOf("OpenLamina");
	fs::path legacy = StoreDirOf("OpenComposite");
	std::error_code ec;
	if (dir.empty() || legacy.empty() || fs::exists(dir, ec) || !fs::is_directory(legacy, ec))
		return dir;
	if (SharedStore::MigrateLegacyDir(legacy, dir)) {
		OOVR_LOGF("Copied the shared store of OpenComposite, %s, to %s", Utf8(legacy).c_str(), Utf8(dir).c_str());
		return dir;
	}
	// Another program that started at the same moment may have done it
	if (fs::exists(dir, ec))
		return dir;
	OOVR_LOGF("Couldn't copy the shared store of OpenComposite, %s, to %s: using it where it is", Utf8(legacy).c_str(), Utf8(dir).c_str());
	return legacy;
}

} // namespace

bool SharedStore::MigrateLegacyDir(const fs::path& legacy, const fs::path& dir)
{
	std::error_code ec;
	fs::create_directories(dir.parent_path(), ec);
	if (ec)
		return false;
	// Into a folder of its own first, which becomes dir in one step: a program that is late can't write the old
	// documents over newer ones, and a half-copied store is never seen
	fs::path temp = dir.parent_path() / (Utf8(dir.filename()) + ".migrating-" + std::to_string(oc_current_pid()));
	fs::remove_all(temp, ec);
	if (!fs::create_directory(temp, ec) || ec)
		return false;
	bool ok = true;
	// Only the documents: state.bin and the lock are this machine's running state, and start again
	for (const char* name : kDocNames) {
		fs::path from = legacy / (std::string(name) + ".json");
		if (!fs::is_regular_file(from, ec))
			continue;
		fs::copy_file(from, temp / from.filename(), fs::copy_options::none, ec);
		if (ec) {
			ok = false;
			break;
		}
	}
	if (ok) {
		fs::rename(temp, dir, ec);
		ok = !ec;
	}
	if (!ok)
		fs::remove_all(temp, ec);
	return ok;
}

struct SharedStore::Header {
	char magic[4]; // "OCST"
	uint32_t format;
	uint64_t generations[8]; // one per StoreDoc
	uint64_t head; // the newest event's seq (0: none yet)
	uint64_t reserved[6];
};
static_assert(sizeof(SharedStore::Header) <= kHeaderSize, "the header fits before the ring");
static_assert(kHeaderSize + SharedStore::kRingSize * sizeof(StoreEvent) <= kStateSize, "the ring fits in state.bin");

// The lock file and the mapping of state.bin
struct SharedStore::Platform {
#ifdef _WIN32
	HANDLE lockFile = INVALID_HANDLE_VALUE;
	HANDLE stateFile = INVALID_HANDLE_VALUE;
	HANDLE mapping = nullptr;
#else
	int lockFd = -1;
	int stateFd = -1;
#endif
	void* view = nullptr;

	bool OpenLock(const fs::path& path)
	{
#ifdef _WIN32
		lockFile = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		return lockFile != INVALID_HANDLE_VALUE;
#else
		lockFd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
		return lockFd >= 0;
#endif
	}

	// Waits for about two seconds at most: a holder that is suspended (in a debugger, say) leaves the store unwritten
	// rather than every other program stuck. Without wait, one try.
	bool Lock(bool wait)
	{
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(wait ? 2 : 0);
		for (;;) {
#ifdef _WIN32
			OVERLAPPED overlapped = {};
			if (LockFileEx(lockFile, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlapped))
				return true;
			if (GetLastError() != ERROR_LOCK_VIOLATION)
				return false;
#else
			if (flock(lockFd, LOCK_EX | LOCK_NB) == 0)
				return true;
			if (errno != EWOULDBLOCK && errno != EINTR)
				return false;
#endif
			if (std::chrono::steady_clock::now() >= deadline)
				return false;
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
	}

	void Unlock()
	{
#ifdef _WIN32
		OVERLAPPED overlapped = {};
		UnlockFileEx(lockFile, 0, 1, 0, &overlapped);
#else
		flock(lockFd, LOCK_UN);
#endif
	}

	// state.bin, made kStateSize long if it's shorter, mapped (two programs doing this at once both get the same file)
	bool MapState(const fs::path& path)
	{
#ifdef _WIN32
		stateFile = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (stateFile == INVALID_HANDLE_VALUE)
			return false;
		// A writable mapping larger than the file extends it
		mapping = CreateFileMappingW(stateFile, nullptr, PAGE_READWRITE, 0, (DWORD)kStateSize, nullptr);
		if (!mapping)
			return false;
		view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, kStateSize);
		return view != nullptr;
#else
		stateFd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
		if (stateFd < 0)
			return false;
		struct stat st = {};
		if (fstat(stateFd, &st) != 0)
			return false;
		// Allocated, not sparse: a full disk fails here rather than with SIGBUS on a write to the mapping
		if ((size_t)st.st_size < kStateSize && posix_fallocate(stateFd, 0, kStateSize) != 0)
			return false;
		view = mmap(nullptr, kStateSize, PROT_READ | PROT_WRITE, MAP_SHARED, stateFd, 0);
		if (view == MAP_FAILED) {
			view = nullptr;
			return false;
		}
		return true;
#endif
	}

	~Platform()
	{
#ifdef _WIN32
		if (view)
			UnmapViewOfFile(view);
		if (mapping)
			CloseHandle(mapping);
		if (stateFile != INVALID_HANDLE_VALUE)
			CloseHandle(stateFile);
		if (lockFile != INVALID_HANDLE_VALUE)
			CloseHandle(lockFile);
#else
		if (view)
			munmap(view, kStateSize);
		if (stateFd >= 0)
			close(stateFd);
		if (lockFd >= 0)
			close(lockFd);
#endif
	}
};

// Holds the store's file lock for a scope
class StoreFileLock {
public:
	explicit StoreFileLock(SharedStore::Platform& p, bool wait = true)
	    : platform(p), held(p.Lock(wait))
	{
	}
	~StoreFileLock()
	{
		if (held)
			platform.Unlock();
	}

private:
	SharedStore::Platform& platform;

public:
	const bool held;
};

SharedStore& SharedStore::Get()
{
	// Never destroyed: the backend writes to it from static destructors at exit, which may run after this one would
	static SharedStore* store = new SharedStore(ChooseDir());
	return *store;
}

SharedStore::SharedStore(const fs::path& directory)
    : dir(directory), platform(std::make_unique<Platform>())
{
	std::error_code ec;
	if (dir.empty())
		return;
	fs::create_directories(dir, ec);
	if (!fs::is_directory(dir, ec))
		return;
	auto unusable = [&](const char* why) {
		std::u8string name = dir.u8string();
		OOVR_LOGF("Shared store in %s not usable: %s", std::string(name.begin(), name.end()).c_str(), why);
	};
	if (!platform->OpenLock(dir / "store.lock") || !platform->MapState(dir / "state.bin")) {
		unusable("its files can't be opened");
		return;
	}

	uint8_t* view = (uint8_t*)platform->view;
	Header* h = (Header*)view;
	auto current = [&] { return memcmp(h->magic, "OCST", 4) == 0 && h->format == kFormat; };
	// The lock only to set up a new or older state.bin: another program's long write doesn't leave this one without a store
	if (!current()) {
		StoreFileLock fileLock(*platform);
		if (!fileLock.held) {
			unusable("another program holds its lock");
			return;
		}
		if (memcmp(h->magic, "OCST", 4) == 0 && h->format > kFormat) {
			unusable("it's a later OpenComposite's");
			return;
		}
		if (!current()) {
			memset(view, 0, kStateSize);
			h->format = kFormat;
			memcpy(h->magic, "OCST", 4);
		}
	}
	state = view;
}

SharedStore::~SharedStore() = default;

SharedStore::Header* SharedStore::header() const
{
	return (Header*)state;
}

StoreEvent* SharedStore::ring() const
{
	return (StoreEvent*)(state + kHeaderSize);
}

fs::path SharedStore::DocPath(StoreDoc doc) const
{
	return dir / (std::string(kDocNames[(int)doc]) + ".json");
}

uint64_t SharedStore::Generation(StoreDoc doc)
{
	return Usable() ? Load(header()->generations[(int)doc]) : 0;
}

Json::Value SharedStore::Read(StoreDoc doc)
{
	if (!Usable())
		return Json::Value(Json::objectValue);
	Refresh(doc);
	std::lock_guard<std::mutex> guard(lock);
	return cache[doc].data;
}

Json::Value SharedStore::ReadAt(StoreDoc doc, const std::vector<std::string>& path)
{
	if (!Usable())
		return Json::Value();
	Refresh(doc);
	std::lock_guard<std::mutex> guard(lock);
	const Json::Value* v = &cache[doc].data;
	for (const std::string& name : path) {
		if (!v->isObject())
			return Json::Value();
		v = v->find(name.data(), name.data() + name.size());
		if (!v)
			return Json::Value();
	}
	return *v;
}

void SharedStore::Refresh(StoreDoc doc)
{
	uint64_t generation;
	{
		std::lock_guard<std::mutex> guard(lock);
		generation = Load(header()->generations[(int)doc]);
		Cached& cached = cache[doc];
		if (cached.loaded && cached.generation == generation)
			return;
	}

	Json::Value data(Json::objectValue);
	std::string text;
	ReadResult result = ReadShared(DocPath(doc), text, 4);
	Json::Value root;
	// A document that doesn't parse reads as empty; one of a later format is read as far as it's understood
	if (result == ReadResult::Ok && ParseDoc(text, root) && root["data"].isObject()) {
		data = root["data"];
	} else if (result == ReadResult::Ok) {
		// Not the document: its last good copy, if there is one
		OOVR_LOGF("Store document %s doesn't parse", DocPath(doc).filename().string().c_str());
		fs::path backup = DocPath(doc);
		backup += ".bak";
		std::string backupText;
		Json::Value backupRoot;
		if (ReadShared(backup, backupText, 4) == ReadResult::Ok && ParseDoc(backupText, backupRoot) && backupRoot["data"].isObject())
			data = backupRoot["data"];
	}

	std::lock_guard<std::mutex> guard(lock);
	Cached& cached = cache[doc];
	if (result == ReadResult::Failed)
		return; // what was read last stays; read again next time
	if (!cached.loaded || cached.generation <= generation)
		cached = { generation, std::move(data), true };
}

bool SharedStore::Update(StoreDoc doc, const std::function<void(Json::Value& data)>& edit, Wait wait)
{
	if (!Usable())
		return false;
	// Not the cache's lock: Read and PollEvents mustn't wait while this waits for another process
	std::unique_lock<std::mutex> writing(writeLock, std::defer_lock);
	if (wait == Wait::Never) {
		if (!writing.try_lock())
			return false;
	} else {
		writing.lock();
	}
	StoreFileLock fileLock(*platform, wait != Wait::Never);
	if (!fileLock.held)
		return false;

	fs::path path = DocPath(doc);
	Json::Value data(Json::objectValue);
	std::string text, backup;
	switch (ReadShared(path, text, 200)) {
	case ReadResult::Failed:
		return false; // there but unreadable: writing now would lose what's in it
	case ReadResult::Missing:
		break;
	case ReadResult::Ok: {
		Json::Value root;
		if (ParseDoc(text, root)) {
			if (root["format"].isUInt() && root["format"].asUInt() > kFormat)
				return false; // a later OpenComposite's: read, never written
			if (root["data"].isObject())
				data = root["data"];
			// The document as it was is the last good copy, for if the next write is cut short
			if (Kept(doc))
				backup = text;
		} else {
			// Kept for a look, and replaced below, starting from the last good copy if there is one
			fs::path bad = path;
			bad += ".bad";
			Replace(path, bad);
			OOVR_LOGF("Store document %s doesn't parse; kept as %s", path.filename().string().c_str(), bad.filename().string().c_str());
			fs::path good = path;
			good += ".bak";
			std::string goodText;
			Json::Value goodRoot;
			if (ReadShared(good, goodText, 4) == ReadResult::Ok && ParseDoc(goodText, goodRoot) && goodRoot["data"].isObject()) {
				data = goodRoot["data"];
				OOVR_LOGF("Store document %s restored from its last good copy", path.filename().string().c_str());
			}
		}
		break;
	}
	}

	try {
		edit(data);
	} catch (const std::exception&) {
		return false; // a type error in the edit (jsoncpp throws): nothing written
	}
	if (!data.isObject())
		return false;

	Json::Value root(Json::objectValue);
	root["format"] = kFormat;
	root["data"] = data;
	Json::StreamWriterBuilder writer;
	writer["indentation"] = "\t";
	fs::path tmp = path;
	tmp += ".tmp";
	std::string out = Json::writeString(writer, root);
	if (out.size() > kMaxDocBytes) {
		OOVR_LOGF("Store document %s not written: it would be %zu bytes", path.filename().string().c_str(), out.size());
		return false;
	}
	if (!backup.empty()) {
		fs::path good = path;
		good += ".bak";
		WriteFile(good, backup, false);
	}
	if (!WriteFile(tmp, out, Kept(doc)) || !Replace(tmp, path)) {
		std::error_code ec;
		fs::remove(tmp, ec);
		return false;
	}

	uint64_t generation = Load(header()->generations[(int)doc]) + 1;
	Store(header()->generations[(int)doc], generation);
	std::lock_guard<std::mutex> guard(lock);
	cache[doc] = { generation, data, true };
	return true;
}

bool SharedStore::PostEvent(uint32_t eventType, const std::array<uint64_t, 4>& data, Wait wait)
{
	if (!Usable())
		return false;
	std::unique_lock<std::mutex> writing(writeLock, std::defer_lock);
	if (wait == Wait::Never) {
		if (!writing.try_lock())
			return false;
	} else {
		writing.lock();
	}
	StoreFileLock fileLock(*platform, wait != Wait::Never);
	if (!fileLock.held)
		return false;

	uint64_t seq = Load(header()->head) + 1;
	{
		// Marked as ours before it's visible, so a poll in this process never takes it for another's
		std::lock_guard<std::mutex> guard(lock);
		ownEvents.insert(seq);
		while (!ownEvents.empty() && *ownEvents.begin() + kRingSize < seq)
			ownEvents.erase(ownEvents.begin());
	}
	StoreEvent& slot = ring()[(size_t)((seq - 1) % kRingSize)];
	// seq 0 while it's written, so a reader never takes half an event for a whole one
	Store(slot.seq, 0);
	slot.pid = (uint32_t)
#ifdef _WIN32
	    GetCurrentProcessId();
#else
	    getpid();
#endif
	slot.eventType = eventType;
	for (int i = 0; i < 4; i++)
		slot.data[i] = data[i];
	Store(slot.seq, seq);
	Store(header()->head, seq);
	return true;
}

std::vector<StoreEvent> SharedStore::PollEvents(uint64_t& cursor, uint32_t* lost)
{
	if (lost)
		*lost = 0;
	std::vector<StoreEvent> events;
	if (!Usable())
		return events;
	std::lock_guard<std::mutex> guard(lock);

	// cursor is the next seq to take
	uint64_t head = Load(header()->head);
	if (cursor == 0 || cursor > head + 1) {
		cursor = head + 1;
		return events;
	}
	uint64_t first = cursor;
	uint32_t missed = 0;
	if (head + 1 - first > kRingSize) {
		missed = (uint32_t)(head + 1 - first - kRingSize);
		first = head + 1 - kRingSize;
	}
	for (uint64_t seq = first; seq <= head; seq++) {
		StoreEvent& slot = ring()[(size_t)((seq - 1) % kRingSize)];
		if (Load(slot.seq) != seq) {
			missed++; // overwritten (or being written) since head was read
			continue;
		}
		StoreEvent copy;
		copy.pid = slot.pid;
		copy.eventType = slot.eventType;
		for (int i = 0; i < 4; i++)
			copy.data[i] = slot.data[i];
		std::atomic_thread_fence(std::memory_order_acquire);
		if (Load(slot.seq) != seq) {
			missed++;
			continue;
		}
		copy.seq = seq;
		if (!ownEvents.count(seq))
			events.push_back(copy);
	}
	cursor = head + 1;
	if (lost)
		*lost = missed;
	return events;
}

uint64_t SharedStore::ProcessStartTime(uint32_t pid)
{
	if (pid == 0)
		return 0;
#ifdef _WIN32
	// Limited query rights: what another user's or an elevated process still grants
	HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!process)
		return 0;
	FILETIME created{}, exited{}, kernel{}, user{};
	DWORD code = 0;
	const bool running = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
	const bool known = GetProcessTimes(process, &created, &exited, &kernel, &user);
	CloseHandle(process);
	if (!running || !known)
		return 0;
	return ((uint64_t)created.dwHighDateTime << 32) | created.dwLowDateTime;
#else
	// Field 22 of /proc/<pid>/stat (clock ticks after boot), after the command name in parentheses
	std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
	std::string stat;
	if (!in || !std::getline(in, stat))
		return 0;
	const size_t close = stat.rfind(')');
	if (close == std::string::npos)
		return 0;
	std::istringstream fields(stat.substr(close + 1));
	std::string field;
	// Fields 3 to 22
	for (int i = 3; i <= 22; i++) {
		if (!(fields >> field))
			return 0;
	}
	char* end = nullptr;
	const unsigned long long ticks = std::strtoull(field.c_str(), &end, 10);
	return end && *end == 0 ? (uint64_t)ticks + 1 : 0; // + 1: a process started at tick 0 is still told apart from none
#endif
}

uint64_t SharedStore::OwnStartTime()
{
	static const uint64_t start = ProcessStartTime(oc_current_pid());
	return start;
}

bool SharedStore::IsSameProcessAlive(uint32_t pid, uint64_t start)
{
	if (start == 0)
		return IsProcessAlive(pid);
	return ProcessStartTime(pid) == start;
}

bool SharedStore::KillProcess(uint32_t pid, uint64_t start)
{
	if (pid == 0 || start == 0)
		return false;
#ifdef _WIN32
	// The start time read from the handle that ends it: the pid can't be another process's by then
	HANDLE process = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!process)
		return false;
	FILETIME created{}, exited{}, kernel{}, user{};
	bool killed = false;
	if (GetProcessTimes(process, &created, &exited, &kernel, &user)
	    && (((uint64_t)created.dwHighDateTime << 32) | created.dwLowDateTime) == start)
		killed = TerminateProcess(process, 1) != 0;
	CloseHandle(process);
	return killed;
#else
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
	// A pidfd names the process itself: checked after it's opened, the signal can't reach another that took the pid
	const int fd = (int)syscall(SYS_pidfd_open, (pid_t)pid, 0);
	if (fd >= 0) {
		const bool same = ProcessStartTime(pid) == start;
		const bool killed = same && syscall(SYS_pidfd_send_signal, fd, SIGKILL, nullptr, 0) == 0;
		close(fd);
		return killed;
	}
#endif
	// Without pidfds: checked just before
	if (ProcessStartTime(pid) != start)
		return false;
	return kill((pid_t)pid, SIGKILL) == 0;
#endif
}

uint64_t SharedStore::KnownProcessStart(uint32_t pid)
{
	if (pid == 0)
		return 0;
	const std::string name = std::to_string(pid);
	std::vector<uint64_t> starts;
	auto add = [&](const Json::Value& v) {
		if (v.isUInt64() && v.asUInt64())
			starts.push_back(v.asUInt64());
	};
	const Json::Value apps = Read(StoreDoc::Applications);
	if (apps["pid_starts"].isObject())
		add(apps["pid_starts"][name]);
	const Json::Value scene = Read(StoreDoc::Scene);
	if (scene.isObject() && scene["pid"].isUInt() && scene["pid"].asUInt() == pid)
		add(scene["start"]);
	const Json::Value overlays = Read(StoreDoc::Overlays);
	if (overlays.isObject() && overlays[name].isObject())
		add(overlays[name]["start"]);
	const uint64_t now = ProcessStartTime(pid);
	for (uint64_t start : starts)
		if (start == now)
			return start;
	return 0;
}

bool SharedStore::OtherLiveProgram(const Json::Value& overlaysDoc, uint32_t self)
{
	if (!overlaysDoc.isObject())
		return false;
	for (const std::string& name : overlaysDoc.getMemberNames()) {
		const unsigned long long pid = std::strtoull(name.c_str(), nullptr, 10);
		if (!pid || pid > 0xffffffffULL || pid == self)
			continue;
		const Json::Value& entry = overlaysDoc[name];
		if (!entry.isObject())
			continue;
		const Json::Value& start = entry["start"];
		if (IsSameProcessAlive((uint32_t)pid, start.isUInt64() ? start.asUInt64() : 0))
			return true;
	}
	return false;
}

bool SharedStore::OtherLiveProgram(const Json::Value& sceneDoc, const Json::Value& overlaysDoc, uint32_t self)
{
	if (sceneDoc.isObject() && sceneDoc["pid"].isUInt()) {
		const uint32_t pid = sceneDoc["pid"].asUInt();
		const Json::Value& start = sceneDoc["start"];
		if (pid && pid != self && IsSameProcessAlive(pid, start.isUInt64() ? start.asUInt64() : 0))
			return true;
	}
	return OtherLiveProgram(overlaysDoc, self);
}

bool SharedStore::IsProcessAlive(uint32_t pid)
{
	if (pid == 0)
		return false;
#ifdef _WIN32
	HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
	if (!process)
		return GetLastError() == ERROR_ACCESS_DENIED; // there, but not ours to open
	bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
	CloseHandle(process);
	return alive;
#else
	return kill((pid_t)pid, 0) == 0 || errno == EPERM;
#endif
}
