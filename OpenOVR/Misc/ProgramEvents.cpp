#include "stdafx.h"

#include "ProgramEvents.h"

#include "Misc/Process.h"
#include "Misc/SharedStore.h"

#include <atomic>
#include <cstring>

static_assert(sizeof(vr::VREvent_Data_t) >= 24, "the data crossing between programs is its first 24 bytes");

namespace {
std::atomic<OcLocalEventSink> localSink{ nullptr };
}

void OcSetLocalEventSink(OcLocalEventSink sink)
{
	localSink = sink;
}

std::array<uint64_t, 4> OcPackEvent(vr::TrackedDeviceIndex_t device, const vr::VREvent_Data_t& data)
{
	std::array<uint64_t, 4> packed{};
	memcpy(packed.data(), &data, 24);
	packed[3] = device;
	return packed;
}

vr::VREvent_t OcUnpackEvent(uint32_t eventType, const uint64_t (&packed)[4])
{
	vr::VREvent_t e{};
	e.eventType = eventType;
	e.trackedDeviceIndex = (vr::TrackedDeviceIndex_t)packed[3];
	memcpy(&e.data, packed, 24);
	return e;
}

void OcPostEvent(vr::EVREventType type, vr::TrackedDeviceIndex_t device, const vr::VREvent_Data_t& data, bool alsoToOtherPrograms)
{
	OcLocalEventSink sink = OcIsPrivateEvent(type) ? nullptr : localSink.load();
	if (sink) {
		vr::VREvent_t e{};
		e.eventType = type;
		e.trackedDeviceIndex = device;
		e.data = data;
		sink(e);
	}
	if (alsoToOtherPrograms)
		SharedStore::Get().PostEvent((uint32_t)type, OcPackEvent(device, data));
}

bool OcPostEventTo(SharedStore& store, uint32_t pid, vr::EVREventType type, vr::TrackedDeviceIndex_t device, const vr::VREvent_Data_t& data)
{
	if (OcIsPrivateEvent(type))
		return false;
	if (pid == oc_current_pid()) {
		OcLocalEventSink sink = localSink.load();
		if (!sink)
			return false;
		vr::VREvent_t e{};
		e.eventType = type;
		e.trackedDeviceIndex = device;
		e.data = data;
		sink(e);
		return true;
	}
	std::array<uint64_t, 4> packed = OcPackEvent(device, data);
	packed[2] = (uint64_t)(uint32_t)type | ((uint64_t)pid << 32);
	return store.PostEvent(OcEvent_ToProgram, packed);
}

std::optional<vr::VREvent_t> OcEventForProgram(const StoreEvent& s, uint32_t selfPid)
{
	if (s.eventType == OcEvent_ToProgram) {
		if ((uint32_t)(s.data[2] >> 32) != selfPid)
			return std::nullopt;
		const uint32_t type = (uint32_t)s.data[2];
		if (OcIsPrivateEvent(type))
			return std::nullopt;
		uint64_t packed[4] = { s.data[0], s.data[1], 0, s.data[3] };
		return OcUnpackEvent(type, packed);
	}
	if (OcIsPrivateEvent(s.eventType))
		return std::nullopt;
	return OcUnpackEvent(s.eventType, s.data);
}

std::optional<std::array<uint64_t, 4>> OcPackOverlayCommand(const OcOverlayCommand& c)
{
	if (c.eventType >= (1u << 24))
		return std::nullopt;
	std::array<uint64_t, 4> packed{};
	packed[0] = c.handle;
	packed[1] = ((uint64_t)c.pid << 32) | ((uint64_t)c.kind << 24) | c.eventType;
	switch (c.kind) {
	case OcOverlayCommand::Alpha:
	case OcOverlayCommand::Width: {
		uint32_t bits = 0;
		memcpy(&bits, &c.number, sizeof(bits));
		packed[2] = bits;
		break;
	}
	case OcOverlayCommand::Flag:
		packed[2] = c.flag;
		packed[3] = c.enabled ? 1 : 0;
		break;
	case OcOverlayCommand::SortOrder:
		packed[2] = c.sortOrder;
		break;
	case OcOverlayCommand::Event:
		memcpy(&packed[2], c.data, sizeof(c.data));
		break;
	default:
		break;
	}
	return packed;
}

OcOverlayCommand OcUnpackOverlayCommand(const uint64_t (&packed)[4])
{
	OcOverlayCommand c;
	c.handle = packed[0];
	c.pid = (uint32_t)(packed[1] >> 32);
	c.kind = (OcOverlayCommand::Kind)(uint8_t)(packed[1] >> 24);
	c.eventType = (uint32_t)(packed[1] & 0xFFFFFF);
	switch (c.kind) {
	case OcOverlayCommand::Alpha:
	case OcOverlayCommand::Width: {
		const uint32_t bits = (uint32_t)packed[2];
		memcpy(&c.number, &bits, sizeof(bits));
		break;
	}
	case OcOverlayCommand::Flag:
		c.flag = (uint32_t)packed[2];
		c.enabled = packed[3] != 0;
		break;
	case OcOverlayCommand::SortOrder:
		c.sortOrder = (uint32_t)packed[2];
		break;
	case OcOverlayCommand::Event:
		memcpy(c.data, &packed[2], sizeof(c.data));
		break;
	default:
		break;
	}
	return c;
}
