#include "stdafx.h"

#include "RemotePointer.h"

#include <climits>
#include <cstdlib>
#include <cstring>
#include <set>

namespace {

// VROverlayInputMethod_Mouse: the lasers send it mouse events
constexpr int kInputMethodMouse = 1;

uint32_t FloatBits(float f)
{
	uint32_t u = 0;
	std::memcpy(&u, &f, sizeof(u));
	return u;
}

float BitsFloat(uint32_t u)
{
	float f = 0;
	std::memcpy(&f, &u, sizeof(f));
	return f;
}

// The pose in the document: {"p": [x, y, z], "q": [x, y, z, w]}; false if it isn't one
bool ReadPose(const Json::Value& v, XrPosef& out)
{
	const Json::Value& p = v["p"];
	const Json::Value& q = v["q"];
	if (!p.isArray() || p.size() != 3 || !q.isArray() || q.size() != 4)
		return false;
	for (Json::ArrayIndex i = 0; i < 3; i++)
		if (!p[i].isNumeric())
			return false;
	for (Json::ArrayIndex i = 0; i < 4; i++)
		if (!q[i].isNumeric())
			return false;
	out.position = { p[0].asFloat(), p[1].asFloat(), p[2].asFloat() };
	out.orientation = { q[0].asFloat(), q[1].asFloat(), q[2].asFloat(), q[3].asFloat() };
	return true;
}

} // namespace

StoreEvent MakeRemotePointerEvent(const RemotePointerEvent& e)
{
	StoreEvent s{};
	s.eventType = OcEvent_RemotePointer;
	s.data[0] = (uint64_t)e.targetPid | ((uint64_t)e.eventType << 32);
	s.data[1] = e.handle;
	s.data[2] = (uint64_t)FloatBits(e.x) | ((uint64_t)FloatBits(e.y) << 32);
	s.data[3] = (uint64_t)(e.device & 0xffff) | ((uint64_t)(e.button & 0xffff) << 16);
	return s;
}

RemotePointerEvent UnpackRemotePointer(const uint64_t (&data)[4])
{
	RemotePointerEvent e{};
	e.targetPid = (uint32_t)(data[0] & 0xffffffff);
	e.eventType = (uint32_t)(data[0] >> 32);
	e.handle = data[1];
	e.x = BitsFloat((uint32_t)(data[2] & 0xffffffff));
	e.y = BitsFloat((uint32_t)(data[2] >> 32));
	e.device = (vr::TrackedDeviceIndex_t)(data[3] & 0xffff);
	if (e.device == 0xffff)
		e.device = vr::k_unTrackedDeviceIndexInvalid;
	e.button = (uint32_t)((data[3] >> 16) & 0xffff);
	return e;
}

size_t PostRemotePointerEvents(const std::vector<StoreEvent>& events)
{
	size_t posted = 0;
	for (const StoreEvent& s : events) {
		// One of OpenLamina's own events: for the other programs only
		vr::VREvent_t e = OcUnpackEvent(s.eventType, s.data);
		if (!SharedStore::Get().PostEvent((uint32_t)e.eventType, OcPackEvent(e.trackedDeviceIndex, e.data), SharedStore::Wait::Never))
			break;
		posted++;
	}
	return posted;
}

void RemotePointer::SetDevicePoses(const std::array<std::optional<XrPosef>, 3>& hmdLeftRight)
{
	devices = hmdLeftRight;
}

void RemotePointer::SetFrameOrigin(vr::ETrackingUniverseOrigin frameOrigin, const XrPosef& seated)
{
	origin = frameOrigin;
	seatedInStanding = seated;
}

std::vector<OOVR_VROverlayIntersectionMaskPrimitive_t> MaskFromJson(const Json::Value& mask)
{
	std::vector<OOVR_VROverlayIntersectionMaskPrimitive_t> out;
	for (const Json::Value& m : mask) {
		if (!m.isObject())
			continue;
		OOVR_VROverlayIntersectionMaskPrimitive_t primitive{};
		if (m["rect"].isArray() && m["rect"].size() == 4) {
			primitive.m_nPrimitiveType = OverlayIntersectionPrimitiveType_Rectangle;
			primitive.m_Primitive.m_Rectangle = { m["rect"][0].asFloat(), m["rect"][1].asFloat(), m["rect"][2].asFloat(), m["rect"][3].asFloat() };
		} else if (m["circle"].isArray() && m["circle"].size() == 3) {
			primitive.m_nPrimitiveType = OverlayIntersectionPrimitiveType_Circle;
			primitive.m_Primitive.m_Circle = { m["circle"][0].asFloat(), m["circle"][1].asFloat(), m["circle"][2].asFloat() };
		} else {
			continue;
		}
		out.push_back(primitive);
	}
	return out;
}

void RemotePointer::Update(const std::array<PointerRay, 2>& rays, const Json::Value& overlaysDoc, uint32_t selfPid, double now, std::vector<StoreEvent>& toPost)
{
	sawDead = false;
	std::vector<PointerTarget> targets;
	std::set<std::pair<uint32_t, uint64_t>> present;
	if (overlaysDoc.isObject()) {
		for (const std::string& name : overlaysDoc.getMemberNames()) {
			char* end = nullptr;
			const unsigned long long pid = std::strtoull(name.c_str(), &end, 10);
			if (!end || *end || pid == 0 || pid > 0xffffffffULL || pid == selfPid)
				continue;
			const Json::Value& start = overlaysDoc[name]["start"];
			if (!isAlive((uint32_t)pid, start.isUInt64() ? start.asUInt64() : 0)) {
				sawDead = true;
				continue;
			}
			const Json::Value& list = overlaysDoc[name]["overlays"];
			if (!list.isArray())
				continue;
			for (const Json::Value& o : list) {
				// Only visible overlays that take mouse input
				if (!o.isObject() || !o["visible"].asBool() || o["input_method"].asInt() != kInputMethodMouse || !o["handle"].isUInt64())
					continue;
				overlay_maths::Placement placement{};
				if (!ReadPose(o["pose"], placement.pose))
					continue;
				// On a device: where that device is this frame
				if (o["relative_to"].isString()) {
					const std::string role = o["relative_to"].asString();
					const int device = role == "hmd" ? 0 : role == "left" ? 1 : role == "right" ? 2 : -1;
					if (device < 0 || !devices[device])
						continue;
					placement.pose = overlay_maths::ComposePoses(*devices[device], placement.pose);
				}
				placement.width = o["width"].asFloat();
				placement.height = o["height"].asFloat();
				placement.curvature = o["curvature"].asFloat();
				if (!(placement.width > 0) || !(placement.height > 0))
					continue;
				if (origin != vr::TrackingUniverseStanding)
					placement.pose = overlay_maths::ChangeOrigin(placement.pose, vr::TrackingUniverseStanding, origin, seatedInStanding);
				const std::pair<uint32_t, uint64_t> key{ (uint32_t)pid, o["handle"].asUInt64() };
				auto it = ids.find(key);
				if (it == ids.end()) {
					it = ids.emplace(key, nextId++).first;
					owners[it->second] = key;
				}
				present.insert(key);
				PointerTarget t{};
				t.handle = it->second;
				t.placement = placement;
				// Units of the overlay's width and height: its owner scales them by its own mouse scale
				t.mouseScale = { { 1.0f, 1.0f } };
				t.flags = o["flags"].asUInt();
				// Another build may have written the whole unsigned number
				const Json::Value& order = o["sort_order"];
				t.sortOrder = order.isInt() ? order.asInt() : order.isUInt() ? INT_MAX - 1 : 0;
				t.mask = MaskFromJson(o["mask"]);
				targets.push_back(t);
			}
		}
	}

	std::vector<PointerEvent> events;
	mouse.Update(rays, targets, now, events);
	for (const PointerEvent& pe : events) {
		auto owner = owners.find(pe.handle);
		if (owner == owners.end())
			continue;
		RemotePointerEvent r{};
		r.targetPid = owner->second.first;
		r.handle = owner->second.second;
		r.eventType = pe.event.eventType;
		r.device = pe.event.trackedDeviceIndex;
		switch (pe.event.eventType) {
		case vr::VREvent_MouseMove:
		case vr::VREvent_MouseButtonDown:
		case vr::VREvent_MouseButtonUp:
			r.x = pe.event.data.mouse.x;
			r.y = pe.event.data.mouse.y;
			r.button = pe.event.data.mouse.button;
			break;
		case vr::VREvent_ScrollDiscrete:
		case vr::VREvent_ScrollSmooth:
			r.x = pe.event.data.scroll.xdelta;
			r.y = pe.event.data.scroll.ydelta;
			break;
		default:
			break;
		}
		toPost.push_back(MakeRemotePointerEvent(r));
	}

	// Overlays gone from the document (their focus was let go above) are forgotten
	for (auto it = ids.begin(); it != ids.end();) {
		if (present.count(it->first)) {
			++it;
			continue;
		}
		owners.erase(it->second);
		it = ids.erase(it);
	}
	lastTargets = std::move(targets);
}

std::optional<XrPosef> RemotePointer::CursorPoint() const
{
	const int hand = mouse.ActiveHand();
	const std::optional<overlay_maths::Hit> hit = mouse.Cursor(hand);
	const vr::VROverlayHandle_t target = mouse.Target(hand);
	if (!hit || target == vr::k_ulOverlayHandleInvalid)
		return std::nullopt;
	for (const PointerTarget& t : lastTargets)
		if (t.handle == target)
			return overlay_maths::PointOn(t.placement, hit->u, hit->v);
	return std::nullopt;
}
