#include "stdafx.h"

#define BASE_IMPL

#include "BaseMailbox.h"

typedef BaseMailbox::MboxErr MboxErr;

MboxErr BaseMailbox::RegisterMailbox(const char* name, OOVR_mbox_handle* handle)
{
	*handle = 123;

	// Ignore it for now
	OOVR_LOGF("Pretending to register mailbox '%s'", name);

	return VR_MBox_None;
}

MboxErr BaseMailbox::UnregisterMailbox(OOVR_mbox_handle mbox)
{
	OOVR_LOGF("Pretending to unregister mailbox ID %d", (int)mbox);
	return VR_MBox_None;
}

MboxErr BaseMailbox::SendMessage(OOVR_mbox_handle mbox, const char* type, const char* message)
{
	OOVR_LOGF("Pretending to send mailbox message, type '%s' contents '%s'", type, message);
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
