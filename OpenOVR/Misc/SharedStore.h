#pragma once

// State that every OpenLamina instance of the user shares: JSON documents (settings, applications, the room
// setup, the scene app, the dashboard, overlays, the reference space offsets set in the runtime) and a ring of events
// between processes. Reading never blocks; changes are a locked read-modify-write, so two programs changing one
// document at once both keep their change.

#include "json/json.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

enum class StoreDoc { Settings, Applications, Chaperone, Scene, Dashboard, Overlays, Offsets };

struct StoreEvent {
	uint64_t seq; // 1, 2, ... across every process; 0 is never an event's
	uint32_t pid; // who posted it
	uint32_t eventType;
	uint64_t data[4];
};
static_assert(sizeof(StoreEvent) == 48, "the ring's events are 48 bytes");

class SharedStore {
public:
	/** The process's store: OC_STORE_DIR if set, else %LOCALAPPDATA%\OpenLamina\store (Linux: $XDG_STATE_HOME or
	 * ~/.local/state, then OpenLamina/store). If that folder doesn't exist yet, the OpenComposite one it replaced (the
	 * same place under OpenComposite) is copied into it first, if there is one. A Windows build in a Wine or Proton
	 * prefix gets that prefix's own %LOCALAPPDATA%, so it shares nothing with programs outside the prefix: setting
	 * OC_STORE_DIR to the same folder (a Z: path to a host folder) in each of them shares the store. Process ids
	 * don't mean the same across that boundary, so the liveness checks of programs on different sides are unreliable. */
	static SharedStore& Get();

	/** Copies the documents (not state.bin or the lock) of the store in legacy to a new folder dir, all at once. False,
	 * with nothing made, if dir exists already or any step fails; legacy is never changed. */
	static bool MigrateLegacyDir(const std::filesystem::path& legacy, const std::filesystem::path& dir);

	/** A store in dir (tests). A folder that can't be made or used leaves the store unusable, never throwing. */
	explicit SharedStore(const std::filesystem::path& dir);
	~SharedStore();
	SharedStore(const SharedStore&) = delete;
	SharedStore& operator=(const SharedStore&) = delete;

	/** A copy of the document's data (an empty object if there's none, it doesn't parse, or the store can't be used),
	 * re-read from disk only when its generation changed */
	Json::Value Read(StoreDoc doc);

	/** A copy of one value in the document: the member at path (each name an object's member), or null if any step
	 * isn't there. Copies only that value, not the whole document. */
	Json::Value ReadAt(StoreDoc doc, const std::vector<std::string>& path);

	/** How long a writer waits for the store's lock, which another program may hold: about two seconds at most, or
	 * not at all, for a writer in the game's frame, which tries again later instead */
	enum class Wait { UpToTwoSeconds, Never };

	/** Locked read-modify-write of the document's data. False if the store can't be used, the file is a newer
	 * format (which is read but never written), or the lock wasn't had in time */
	bool Update(StoreDoc doc, const std::function<void(Json::Value& data)>& edit, Wait wait = Wait::UpToTwoSeconds);

	/** Update from the game's frame: the edit is made on the process's writer thread (one per store), so the frame
	 * never waits for the store's lock or its file. A later edit with the same non-empty key replaces one not yet
	 * made (only the last of them is made); edits without a key are all made, in order. Reads in this process see
	 * the edits not yet made, and an Update of the document makes them first. The edit is kept and called later,
	 * maybe more than once (each read before it's made): it captures by value. */
	void UpdateLater(StoreDoc doc, const std::string& key, std::function<void(Json::Value& data)> edit);

	/** Waits, a few seconds at most, until the edits UpdateLater was given so far are made (or given up on) */
	void Flush();

	/** Changes with every Update of the document, in any process; 0 if the store can't be used */
	uint64_t Generation(StoreDoc doc);

	/** Puts an event in the ring for the other processes (and other stores in this one). False if it wasn't put there:
	 * the store can't be used, or the lock wasn't had in time. */
	bool PostEvent(uint32_t eventType, const std::array<uint64_t, 4>& data, Wait wait = Wait::UpToTwoSeconds);

	/** The events other stores posted after cursor, which moves to the newest. Cursor 0 starts at the current head
	 * (nothing returned). More than the ring holds since cursor: the oldest are gone, and *lost says how many. */
	std::vector<StoreEvent> PollEvents(uint64_t& cursor, uint32_t* lost = nullptr);

	static bool IsProcessAlive(uint32_t pid);

	/** When a process started: a number that differs between two processes that had the same pid (pids are reused),
	 * 0 if it can't be told (the process isn't running, or isn't ours to ask) */
	static uint64_t ProcessStartTime(uint32_t pid);
	/** This process's ProcessStartTime */
	static uint64_t OwnStartTime();
	/** Whether pid is running and is the process that started at start; with start 0 (an entry written without it),
	 * whether any process with that pid is running */
	static bool IsSameProcessAlive(uint32_t pid, uint64_t start);
	/** Ends pid at once if it's the process that started at start (not 0), checked as near to the kill as the system
	 * allows; false if it isn't, or couldn't be ended */
	static bool KillProcess(uint32_t pid, uint64_t start);

	/** Keeps this library loaded for good, before a thread of its own starts in it: the game's openvr_api may unload
	 * the library at VR_Shutdown while the process goes on, and the thread would then wake in unmapped code */
	static void StayLoaded();
	/** The start time the store has for a pid (the Applications document's processes, the scene's, the overlay
	 * programs'), if it's still that process; 0 for a pid the store doesn't know or that's gone */
	uint64_t KnownProcessStart(uint32_t pid);
	/** Whether a program other than self has an entry in the Overlays document and is still running */
	static bool OtherLiveProgram(const Json::Value& overlaysDoc, uint32_t self);
	/** The same, or the Scene document's scene application (other than self) is still running */
	static bool OtherLiveProgram(const Json::Value& sceneDoc, const Json::Value& overlaysDoc, uint32_t self);

	static constexpr uint32_t kFormat = 1;
	static constexpr uint32_t kRingSize = 256;

	// SharedStore.cpp's: the lock file and mapping, and state.bin's header
	struct Platform;
	struct Header;

private:

	bool Usable() const { return state != nullptr; }
	/** Brings the document's cached copy up to date; the caller then reads it under lock */
	void Refresh(StoreDoc doc);
	std::filesystem::path DocPath(StoreDoc doc) const;
	Header* header() const;
	StoreEvent* ring() const;

	std::filesystem::path dir;
	std::unique_ptr<Platform> platform;
	uint8_t* state = nullptr; // state.bin, mapped

	std::timed_mutex writeLock; // this store's writers, which share its lock file handle
	std::mutex lock; // the cache and ownEvents
	struct Cached {
		uint64_t generation = 0;
		Json::Value data{ Json::objectValue };
		bool loaded = false;
	};
	std::map<StoreDoc, Cached> cache;
	std::set<uint64_t> ownEvents; // seqs this store posted, which its own polls skip

	// UpdateLater's edits not yet made, and the writer thread that makes them
	struct Later;
	std::unique_ptr<Later> later;
	/** The document's edits not yet made, with their ids */
	std::vector<std::pair<uint64_t, std::function<void(Json::Value&)>>> PendingEdits(StoreDoc doc);
	/** Update, making the pending edits first (and, with pendingOnly, writing nothing if there are none) */
	bool WriteUpdate(StoreDoc doc, const std::function<void(Json::Value& data)>* edit, Wait wait, bool pendingOnly);
	void WriterLoop();
};
