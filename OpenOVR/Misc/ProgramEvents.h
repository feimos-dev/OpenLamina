#pragma once

// Events from calls: queued for this program's IVRSystem, and for every other OpenComposite program of the
// user through the store (SharedStore's event ring). Other programs' events are taken off the ring in PollNextEvent.

#include "generated/interfaces/vrtypes.h"

#include <array>
#include <cstdint>
#include <optional>

class SharedStore;
struct StoreEvent;

// OpenLamina's own events between programs, in the vendor range: never queued for a program's IVRSystem
constexpr uint32_t OcEvent_First = vr::VREvent_VendorSpecific_Reserved_Start + 1;
// The pointer on another program's overlay (RemotePointer.h): for that program, which queues it on the overlay
constexpr uint32_t OcEvent_RemotePointer = OcEvent_First;
// An event for one program only: data[0..1] the event's data's first 16 bytes, data[2] its type (low 32 bits) and the
// process id it's for (high 32 bits), data[3] the device
constexpr uint32_t OcEvent_ToProgram = OcEvent_First + 1;
// A change to an overlay of the one program it's for, asked for by another program through FindOverlay's handle for
// it: that program applies it (OcOverlayCommand)
constexpr uint32_t OcEvent_OverlayCommand = OcEvent_First + 2;
constexpr uint32_t OcEvent_Last = vr::VREvent_VendorSpecific_Reserved_Start + 99;
inline bool OcIsPrivateEvent(uint32_t eventType)
{
	return eventType >= OcEvent_First && eventType <= OcEvent_Last;
}

/** Queues the event for this program (once it has an IVRSystem; not OpenLamina's own events) and, if
 * alsoToOtherPrograms, for the others */
void OcPostEvent(vr::EVREventType type, vr::TrackedDeviceIndex_t device, const vr::VREvent_Data_t& data, bool alsoToOtherPrograms);

/** Queues the event for the one program with process id pid: this program's queue if it's this one, else the other's
 * through the store. Only the data's first 16 bytes reach another program. False if it couldn't be posted. */
bool OcPostEventTo(SharedStore& store, uint32_t pid, vr::EVREventType type, vr::TrackedDeviceIndex_t device, const vr::VREvent_Data_t& data);

/** The event a program with process id selfPid takes from another program's event in the store: a public one, or
 * one addressed to it. Nothing for OpenLamina's own events or another program's addressed ones. */
std::optional<vr::VREvent_t> OcEventForProgram(const StoreEvent& s, uint32_t selfPid);

/** Where this program's events go: BaseSystem sets it while it lives, nullptr for nowhere */
using OcLocalEventSink = void (*)(const vr::VREvent_t& event);
void OcSetLocalEventSink(OcLocalEventSink sink);

/** An event as it crosses between programs: data[0..2] its data's first 24 bytes, data[3] the device; and back.
 * What a payload holds past its first 24 bytes (a notification's text, a keyboard's characters) doesn't cross: the
 * other program gets the event with that part zeroed. */
std::array<uint64_t, 4> OcPackEvent(vr::TrackedDeviceIndex_t device, const vr::VREvent_Data_t& data);
vr::VREvent_t OcUnpackEvent(uint32_t eventType, const uint64_t (&packed)[4]);

/** What another program asks of an overlay (OcEvent_OverlayCommand). Show and Hide take nothing; Alpha and Width a
 * float (number); Flag the flag's bit number (flag) and whether it's set (enabled); SortOrder a number; Event an overlay
 * event of eventType with the first 16 bytes of its data (data), as PostOverlayEvent posts it. */
struct OcOverlayCommand {
	enum Kind : uint8_t {
		Show = 1,
		Hide,
		Alpha,
		Width,
		Flag,
		SortOrder,
		Event,
	};
	uint32_t pid = 0; // the program whose overlay it is
	uint64_t handle = 0; // the overlay's handle in that program
	Kind kind = Show;
	uint32_t eventType = 0; // an Event's, below 2^24
	float number = 0;
	uint32_t flag = 0;
	bool enabled = false;
	uint32_t sortOrder = 0;
	uint8_t data[16] = {};
};
/** A command as it crosses between programs: data[0] the handle, data[1] the pid (high 32 bits), the kind (the next 8)
 * and an event's type (the low 24), data[2..3] what it takes; and back. Nothing for an event type of 2^24 or more. */
std::optional<std::array<uint64_t, 4>> OcPackOverlayCommand(const OcOverlayCommand& command);
OcOverlayCommand OcUnpackOverlayCommand(const uint64_t (&packed)[4]);
