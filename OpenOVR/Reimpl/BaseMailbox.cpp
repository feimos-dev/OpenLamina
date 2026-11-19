#include "stdafx.h"

#define BASE_IMPL

#include "BaseMailbox.h"

typedef BaseMailbox::MboxErr MboxErr;

MboxErr BaseMailbox::RegisterMailbox(const char* name, OOVR_mbox_handle* handle)
{
	if (!name || !handle) {
		if (handle)
			*handle = 0;
		return VR_MBox_NoMessage;
	}

	// The same name gives the same handle for as long as it's registered
	std::lock_guard<std::mutex> lock(mutex);
	auto it = handles.find(name);
	if (it == handles.end())
		it = handles.emplace(name, nextHandle++).first;
	*handle = it->second;

	OOVR_LOGF("Registered mailbox '%s' as %d", name, (int)*handle);

	return VR_MBox_None;
}

MboxErr BaseMailbox::UnregisterMailbox(OOVR_mbox_handle mbox)
{
	std::lock_guard<std::mutex> lock(mutex);
	for (auto it = handles.begin(); it != handles.end(); ++it) {
		if (it->second == mbox) {
			handles.erase(it);
			break;
		}
	}
	OOVR_LOGF("Pretending to unregister mailbox ID %d", (int)mbox);
	return VR_MBox_None;
}

MboxErr BaseMailbox::SendMessage(OOVR_mbox_handle mbox, const char* type, const char* message)
{
	OOVR_LOGF("Pretending to send mailbox message, type '%s' contents '%s'", type ? type : "(null)", message ? message : "(null)");
	return VR_MBox_None;
}

MboxErr BaseMailbox::ReadMessage(OOVR_mbox_handle mboxHandle, char* outBuf, uint32_t outBufLen, uint32_t* msgLen)
{
	// Read message function
	// If we have a message available (IDK what the mailbox is even supposed to do), try to read a message.
	// If the message fits within the output buffer, copy it in and return 0.
	// Otherwise if it doesn't fit, return 2.
	// In both cases, set msgLen
	// If we don't have a message, return an unknown error code other than 0 and 2 (HL:A doesn't care).

	uint32_t lenScratch = 0;
	if (!msgLen) {
		msgLen = &lenScratch;
	}
	*msgLen = 0;

	if (!readySent) {
		static const char msg[] = R"({ "type": "ready" })";
		const uint32_t len = (uint32_t)(sizeof(msg) - 1);
		*msgLen = len;

		if (!outBuf || outBufLen < len + 1) {
			return VR_MBox_BufferTooShort;
		}
		memcpy(outBuf, msg, len + 1);
		readySent = true;

		OOVR_LOGF("Sending fake ready message '%s'", msg);
		return VR_MBox_None;
	}

	// For now, just say we don't have a message
	return VR_MBox_NoMessage;
}
