#pragma once

#include "../BaseCommon.h"
#include "custom_interfaces/IVRMailbox_001.h"

#include <map>
#include <mutex>
#include <string>

typedef vr::IVRMailbox_001::mbox_handle OOVR_mbox_handle;

class BaseMailbox {
public:
	enum MboxErr {
		VR_MBox_None = 0,
		VR_MBox_NoMessage = 1,
		VR_MBox_BufferTooShort = 2,
		VR_MBox_InvalidHandle = 3, // a mailbox that isn't registered
		VR_MBox_InvalidParameter = 4, // a name, address or out-pointer missing
	};

	// TODO build up names and comments for these and their types

	MboxErr RegisterMailbox(const char* name, OOVR_mbox_handle* handle);

	MboxErr UnregisterMailbox(OOVR_mbox_handle mbox);

	MboxErr SendMessage(OOVR_mbox_handle mbox, const char* type, const char* message);

	MboxErr ReadMessage(OOVR_mbox_handle mbox, char* outBuf, uint32_t outBufLen, uint32_t* msgLen);

private:
	std::mutex mutex;
	std::map<std::string, OOVR_mbox_handle> handles;
	// Not a small number, as SteamVR's aren't, so one a caller made up isn't taken for a mailbox
	OOVR_mbox_handle nextHandle = 0x800;
	// The ready message is delivered once per interface instance
	bool readySent = false;

	bool Registered(OOVR_mbox_handle mbox) const;
};

typedef BaseMailbox::MboxErr OOVR_vrmb_typeb;
