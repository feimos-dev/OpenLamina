#pragma once
#include "BaseCommon.h"

#include <string>

namespace Json {
class Value;
}

class BaseClientCore {
public:
	static bool CheckAppEnabled();
	// The runtime apps-config.json chooses for a program (1 OpenComposite, 2 SteamVR, or another number the file
	// holds); OpenComposite when nothing usable is set
	static int _ConfiguredRuntime(const Json::Value& config, const std::string& appPath);
	static std::string GetAlternativeRuntimePath();
	static inline vr::EVRApplicationType appType = vr::VRApplication_Max;
	// The folder of OpenComposite's DLL, ending with a separator
	static std::wstring GetDllDir();
	// The program's executable, UTF-8: the key of its entries in applist.json and apps-config.json
	static std::string GetAppPath();

	/** Initializes the system */
	vr::EVRInitError Init(vr::EVRApplicationType eApplicationType, const char* pStartupInfo);

	/** cleans up everything in vrclient.dll and prepares the DLL to be unloaded */
	void Cleanup();

	/** checks to see if the specified interface/version is supported in this vrclient.dll */
	vr::EVRInitError IsInterfaceVersionValid(const char* pchInterfaceVersion);

	/** Retrieves any interface from vrclient.dll */
	void* GetGenericInterface(const char* pchNameAndVersion, vr::EVRInitError* peError);

	/** Returns true if any driver has an HMD attached. Can be called outside of Init/Cleanup */
	bool BIsHmdPresent();

	/** Returns an English error string from inside vrclient.dll which might be newer than the API DLL */
	const char* GetEnglishStringForHmdError(vr::EVRInitError eError);

	/** Returns an error symbol from inside vrclient.dll which might be newer than the API DLL */
	const char* GetIDForVRInitError(vr::EVRInitError eError);
};
