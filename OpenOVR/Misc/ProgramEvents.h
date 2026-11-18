#pragma once

// Events from calls: queued for this program's IVRSystem, and for every other OpenComposite program of the
// user through the store (SharedStore's event ring). Other programs' events are taken off the ring in PollNextEvent.

#include "generated/interfaces/vrtypes.h"

#include <array>
#include <cstdint>

// OpenLamina's own events between programs, in the vendor range: never queued for a program's IVRSystem
constexpr uint32_t OcEvent_First = vr::VREvent_VendorSpecific_Reserved_Start + 1;
// The pointer on another program's overlay (RemotePointer.h): for that program, which queues it on the overlay
constexpr uint32_t OcEvent_RemotePointer = OcEvent_First;
constexpr uint32_t OcEvent_Last = vr::VREvent_VendorSpecific_Reserved_Start + 99;
inline bool OcIsPrivateEvent(uint32_t eventType)
{
	return eventType >= OcEvent_First && eventType <= OcEvent_Last;
}

/** Queues the event for this program (once it has an IVRSystem; not OpenLamina's own events) and, if
 * alsoToOtherPrograms, for the others */
void OcPostEvent(vr::EVREventType type, vr::TrackedDeviceIndex_t device, const vr::VREvent_Data_t& data, bool alsoToOtherPrograms);

/** Where this program's events go: BaseSystem sets it while it lives, nullptr for nowhere */
using OcLocalEventSink = void (*)(const vr::VREvent_t& event);
void OcSetLocalEventSink(OcLocalEventSink sink);

/** An event as it crosses between programs: data[0..2] its data's first 24 bytes, data[3] the device; and back */
std::array<uint64_t, 4> OcPackEvent(vr::TrackedDeviceIndex_t device, const vr::VREvent_Data_t& data);
vr::VREvent_t OcUnpackEvent(uint32_t eventType, const uint64_t (&packed)[4]);
