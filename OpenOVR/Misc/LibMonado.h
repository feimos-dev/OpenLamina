#pragma once

// libmonado: Monado's client library for what OpenXR can't ask, found through the active runtime's manifest
// and loaded at run time. Only when the runtime is Monado and its libmonado's major version is 1; without it every
// answer is OpenComposite's own, as before.

#include "generated/interfaces/vrtypes.h"

#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>

class LibMonado {
public:
	/** The current OpenXR instance's: loaded on first use after the instance exists, if its runtime is Monado; one for
	 * the same runtime name and instance is kept, another instance gets its own. Without an instance: one with no
	 * libmonado. */
	static LibMonado& Get();

	/** Loads libmonado for a runtime of that name (tests); OC_TEST_LIBMONADO_PATH, if set, is the library */
	explicit LibMonado(const std::string& runtimeName);
	~LibMonado();
	LibMonado(const LibMonado&) = delete;
	LibMonado& operator=(const LibMonado&) = delete;

	bool Available() const { return root != nullptr; }

	struct Battery {
		bool present = false, charging = false;
		float charge = 0; // 0 to 1
	};
	/** The battery of the device of that name or serial; nothing without libmonado or such a device */
	std::optional<Battery> BatteryFor(const std::string& deviceName, const std::string& serial);
	/** The battery of the device in a role: "head", "left", "right" */
	std::optional<Battery> BatteryForRole(const char* role);
	/** The name of the device in a role */
	std::optional<std::string> DeviceForRole(const char* role);
	struct DeviceInfo {
		std::string name, serial; // either may be empty if libmonado gives none
	};
	/** The name and serial of the device in a role; nothing without libmonado or such a device */
	std::optional<DeviceInfo> DeviceInfoForRole(const char* role);
	/** libmonado's mnd_reference_space_type_t */
	static constexpr int kReferenceSpaceStage = 3;
	/** Moves a reference space: the offset is the space's origin in the runtime's own space (the room setup's zero poses).
	 * False without libmonado, or if Monado refuses it (logged once per space type). */
	bool SetReferenceSpaceOffset(int referenceSpaceType, const vr::HmdMatrix34_t& offset);
	/** The offset a reference space has now; nothing without libmonado 1.3 */
	std::optional<vr::HmdMatrix34_t> GetReferenceSpaceOffset(int referenceSpaceType);

	/** The active OpenXR runtime's manifest, as the loader finds it (XR_RUNTIME_JSON, else the registry or the XDG
	 * config folders) */
	static std::optional<std::filesystem::path> ActiveRuntimeManifest();
	/** XR_RUNTIME_JSON as a path (read wide on Windows), whatever the process's elevation; nothing if it isn't set */
	static std::optional<std::filesystem::path> RuntimeManifestFromEnvironment();
	/** The manifest's MND_libmonado_path, resolved against its folder */
	static std::optional<std::filesystem::path> LibraryFromManifest(const std::filesystem::path& manifest);

	struct Api;

private:
	std::optional<uint32_t> DeviceIndex(const std::string& deviceName, const std::string& serial);
	/** The battery of the device at an index (lock held): a failed call (Monado's answer for a device without battery
	 * status) is no battery */
	Battery BatteryAt(uint32_t index);

	void* library = nullptr;
	void* root = nullptr; // mnd_root_t*
	Api* api = nullptr;
	std::mutex lock;
	// Battery answers kept a second: games ask every frame, and each is a call into the service
	std::map<std::string, std::pair<std::chrono::steady_clock::time_point, std::optional<Battery>>> batteryCache;
	std::map<std::string, std::pair<std::chrono::steady_clock::time_point, std::optional<DeviceInfo>>> deviceInfoCache;
	// The reference space types whose refused offset has been logged
	std::set<int> refusalsLogged;
};
