#include "stdafx.h"
#define BASE_IMPL
#include "BaseControlPanel.h"

#include "BaseApplications.h"
#include "Misc/Dashboard.h"
#include "Misc/ProgramEvents.h"
#include "Misc/SharedStore.h"

// These functions are used in HL:A - See #176
// Nooping both of these should be fine.
void BaseControlPanel::SetDashBoardUserToggleEnabled(bool status)
{
}
OOVR_EVR_ControlPanel_Error BaseControlPanel::RegisterExternalWebRoot()
{
	return VR_ControlPanelError_None;
}

// Unknown functions
// SteamVR's own (undocumented) control panel calls: nothing that a game relies on
#define UKN_FUNC(id)                                                       \
	void BaseControlPanel::UknFunc##id()                                   \
	{                                                                      \
		OC_UNSUPPORTED_VOID("IVRControlPanel: unknown function " #id); \
	}

UKN_FUNC(00);
UKN_FUNC(01);
UKN_FUNC(02);
UKN_FUNC(03);
UKN_FUNC(04);
UKN_FUNC(05);
UKN_FUNC(06);
UKN_FUNC(07);
UKN_FUNC(08);
UKN_FUNC(09);
UKN_FUNC(10);
UKN_FUNC(11);
UKN_FUNC(12);
UKN_FUNC(13);
UKN_FUNC(14);
UKN_FUNC(16);
UKN_FUNC(17);
UKN_FUNC(18);
UKN_FUNC(19);
UKN_FUNC(20);
UKN_FUNC(21);
UKN_FUNC(22);
UKN_FUNC(23);
UKN_FUNC(24);
UKN_FUNC(25);
UKN_FUNC(27);
UKN_FUNC(28);
UKN_FUNC(29);

#include "BaseClientCore.h"
#include "BaseDriverManager.h"
#include "BaseResources.h"
#include "Misc/Process.h"
#include "BaseSystem.h"
#include "generated/static_bases.gen.h"

static uint32_t EmptyString(char* pchBuffer, uint32_t unBufferLen)
{
	if (pchBuffer && unBufferLen)
		pchBuffer[0] = '\0';
	return 0;
}

// The one driver BaseDriverManager lists
uint32_t BaseControlPanel::GetDriverCount()
{
	return 1;
}

uint32_t BaseControlPanel::GetDriverId(uint32_t unDriverIndex, char* pchBuffer, uint32_t unBufferLen)
{
	if (unDriverIndex != 0)
		return EmptyString(pchBuffer, unBufferLen);
	// As BaseDriverManager::GetDriverName: the size the name needs, with its terminator; copied when it fits
	const std::string name = BaseDriverManager::DriverName();
	const uint32_t needed = (uint32_t)name.size() + 1;
	if (pchBuffer && unBufferLen >= needed)
		memcpy(pchBuffer, name.c_str(), needed);
	else if (pchBuffer && unBufferLen)
		pchBuffer[0] = '\0';
	return needed;
}

uint32_t BaseControlPanel::GetDriverDisplayCount(const char* pchDriverId)
{
	return 0;
}

uint32_t BaseControlPanel::GetDriverDisplayId(const char* pchDriverId, uint32_t unDisplayIndex, char* pchBuffer, uint32_t unBufferLen)
{
	return EmptyString(pchBuffer, unBufferLen);
}

uint32_t BaseControlPanel::GetDriverDisplayModelNumber(const char* pchDriverId, const char* pchDisplayId, char* pchBuffer, uint32_t unBufferLen)
{
	return EmptyString(pchBuffer, unBufferLen);
}

uint32_t BaseControlPanel::GetDriverDisplaySerialNumber(const char* pchDriverId, const char* pchDisplayId, char* pchBuffer, uint32_t unBufferLen)
{
	return EmptyString(pchBuffer, unBufferLen);
}

void* BaseControlPanel::GetCurrentDisplayInterface(const char* pchHmdInterfaceVersion)
{
	// The interface asked for, as VR_GetGenericInterface gives it (null for one OpenComposite doesn't offer)
	if (!pchHmdInterfaceVersion)
		return nullptr;
	vr::EVRInitError err = vr::VRInitError_None;
	return GetCreateBaseClientCore()->GetGenericInterface(pchHmdInterfaceVersion, &err);
}

void* BaseControlPanel::GetCurrentCompositorInterface(const char* pchInterfaceVersion)
{
	return GetCurrentDisplayInterface(pchInterfaceVersion);
}

uint32_t BaseControlPanel::LoadSharedResource(const char* pchResourceName, char* pchBuffer, uint32_t unBufferLen)
{
	// BaseResources keeps no state, so the same reading as IVRResources
	return BaseResources().LoadSharedResource(pchResourceName, pchBuffer, unBufferLen);
}

float BaseControlPanel::GetIPD()
{
	return BaseSystem::SGetIpd();
}

void BaseControlPanel::SetIPD(float fIPD)
{
	OC_UNSUPPORTED_VOID("SetIPD (IVRControlPanel_001): the IPD is the headset's");
}

bool BaseControlPanel::QuitProcess(uint32_t pidProcessToQuit)
{
	// SteamVR sends the process a quit request and answers true, whatever the process. This process gets VREvent_Quit,
	// and so does another OpenLamina program (one listed in the store); any other process isn't OpenLamina's to reach
	vr::VREvent_Data_t data{};
	data.process.pid = pidProcessToQuit;
	if (pidProcessToQuit == oc_current_pid()) {
		OcPostEventTo(SharedStore::Get(), pidProcessToQuit, vr::VREvent_Quit, vr::k_unTrackedDeviceIndexInvalid, data);
		return true;
	}
	char key[256];
	if (pidProcessToQuit && BaseApplications().GetApplicationKeyByProcessId(pidProcessToQuit, key, sizeof(key)) == VRApplicationError_None) {
		OcPostEventTo(SharedStore::Get(), pidProcessToQuit, vr::VREvent_Quit, vr::k_unTrackedDeviceIndexInvalid, data);
		return true;
	}
	OC_UNSUPPORTED(true, "QuitProcess (IVRControlPanel_001): a process that isn't an OpenLamina program isn't OpenLamina's to quit");
}

bool BaseControlPanel::ForceQuitProcess(uint32_t pidProcessToQuit)
{
	OC_UNSUPPORTED(false, "ForceQuitProcess (IVRControlPanel_001): other processes aren't OpenComposite's to quit");
}

uint32_t BaseControlPanel::StartVRProcess(const char* pchExecutable, const char** pchArguments, uint32_t unArgumentCount, const char* pchWorkingDirectory)
{
	// 0: no process started
	OC_UNSUPPORTED(0, "StartVRProcess (IVRControlPanel_001): starting VR processes is the OpenXR runtime's");
}

void BaseControlPanel::SetMasterProcessToThis()
{
	// There's no master process to hand over: each OpenComposite app is its own
}

void BaseControlPanel::StartAutolaunchOverlays()
{
	BaseApplications apps;
	::StartAutolaunchOverlays(apps);
}

void BaseControlPanel::AbortTransition()
{
	// No transition between scene apps is in progress (each runs on its own)
}
