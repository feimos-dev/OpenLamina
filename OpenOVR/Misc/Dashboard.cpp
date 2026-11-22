#include "stdafx.h"

#include "Dashboard.h"

#include "Keyboard/VRKeyboard.h"
#include "RemotePointer.h"
#include "SharedStore.h"

#include "Reimpl/BaseApplications.h"

#include <cstdlib>

namespace {

constexpr int kMargin = 20;
constexpr int kTabWidth = 220, kTabHeight = 60, kTabGap = 12;

// As the keyboard converts: a character beyond 16 bits doesn't spoil the rest of the label
std::wstring Wide(const std::string& utf8)
{
	return VRKeyboard::Utf8ToWide(utf8);
}

} // namespace

Dashboard::Rect Dashboard::TabRect(size_t i)
{
	return Rect{ kMargin + (int)i * (kTabWidth + kTabGap), kMargin, kTabWidth, kTabHeight };
}

Dashboard::Rect Dashboard::QuitRect()
{
	return Rect{ kWidth - kMargin - kTabWidth, kHeight - kMargin - kTabHeight, kTabWidth, kTabHeight };
}

int Dashboard::OnInput(bool leftDown, bool rightDown, double now)
{
	const bool down = leftDown || rightDown;
	if (button == Button::Off) {
		held = down;
		return kNoHand;
	}
	int recentre = kNoHand;
	if (down && !held) {
		heldSince = now;
		heldHand = leftDown ? 0 : 1;
		toggledThisPress = false;
		// Menu: each press opens or closes it
		if (button == Button::Menu) {
			open = !open;
			toggledThisPress = true;
		}
	}
	// MenuLong: held long enough, once a press
	if (down && button == Button::MenuLong && !toggledThisPress && now - heldSince >= kLongPress) {
		open = !open;
		toggledThisPress = true;
	}
	if (button == Button::PressOrHold) {
		if (down && !toggledThisPress && now - heldSince >= kRecentreHold) {
			// Held long enough: a recentre now, once a press, from the hand that began it if it's still the one held
			const bool beganHeld = heldHand == 0 ? leftDown : rightDown;
			recentre = beganHeld ? heldHand : 1 - heldHand;
			toggledThisPress = true;
		} else if (!down && held && !toggledThisPress && now - heldSince < kRecentreHold) {
			// Let go before that: a press. Let go later with no recentre seen (no input while it was held, as when the
			// controller goes away): nothing
			open = !open;
		}
	}
	held = down;
	return recentre;
}

void Dashboard::SetOpen(bool isOpen)
{
	open = isOpen;
}

std::vector<Dashboard::Tab> Dashboard::Tabs(const Json::Value& stored) const
{
	const Json::Value overlaysDoc = CheckedOverlaysDoc(stored);
	std::vector<Tab> tabs;
	if (!overlaysDoc.isObject())
		return tabs;
	for (const std::string& name : overlaysDoc.getMemberNames()) {
		char* end = nullptr;
		const unsigned long long pid = std::strtoull(name.c_str(), &end, 10);
		if (!end || *end || pid == 0 || pid > 0xffffffffULL)
			continue;
		const Json::Value& entry = overlaysDoc[name];
		const uint64_t start = entry["start"].isUInt64() ? entry["start"].asUInt64() : 0;
		if (!(isAlive ? isAlive((uint32_t)pid, start) : SharedStore::IsSameProcessAlive((uint32_t)pid, start)))
			continue;
		for (const Json::Value& o : entry["overlays"]) {
			if (!o.isObject() || !o["dashboard"].asBool() || o["thumbnail"].asBool() || !o["handle"].isUInt64())
				continue;
			std::string label = o["name"].asString();
			if (label.empty())
				label = entry["app"].asString();
			// More than a tab can show, which Draw shortens a character at a time (a cut character is left out)
			if (label.size() > 256)
				label.resize(256);
			tabs.push_back(Tab{ (uint32_t)pid, o["handle"].asUInt64(), Wide(label) });
		}
	}
	return tabs;
}

void Dashboard::Draw(Canvas& canvas, const Json::Value& dashboardDoc, const Json::Value& overlaysDoc) const
{
	drawnTabs = Tabs(overlaysDoc);
	const uint32_t activePid = dashboardDoc["active_pid"].isUInt() ? dashboardDoc["active_pid"].asUInt() : 0;
	const uint64_t activeOverlay = dashboardDoc["active_overlay"].isUInt64() ? dashboardDoc["active_overlay"].asUInt64() : 0;

	// A border around the panel, which the chosen program draws in the clear middle
	canvas.Clear(0x00000000);
	const uint32_t frame = 0x1e2228e8;
	const int top = kMargin + kTabHeight + kMargin, bottom = kHeight - (kMargin + kTabHeight + kMargin);
	canvas.FillRect(0, 0, kWidth, top, frame, 0);
	canvas.FillRect(0, bottom, kWidth, kHeight - bottom, frame, 0);
	canvas.FillRect(0, top, kMargin, bottom - top, frame, 0);
	canvas.FillRect(kWidth - kMargin, top, kMargin, bottom - top, frame, 0);

	const int line = Canvas::LineHeight();
	auto label = [&](const Rect& r, const std::wstring& text, uint32_t colour) {
		canvas.FillRect(r.x, r.y, r.w, r.h, colour, 10);
		std::wstring shown = text;
		while (!shown.empty() && canvas.TextWidth(shown) > r.w - 16)
			shown.pop_back();
		canvas.Text(r.x + (r.w - canvas.TextWidth(shown)) / 2, r.y + (r.h - line) / 2, shown, 0xffffffff);
	};
	for (size_t i = 0; i < drawnTabs.size(); i++) {
		const Rect r = TabRect(i);
		if (r.x + r.w > kWidth - kMargin)
			break; // no room for more
		const bool active = drawnTabs[i].pid == activePid && drawnTabs[i].handle == activeOverlay;
		label(r, drawnTabs[i].label, active ? 0x3a6ea5ff : 0x3a404cff);
	}
	if (drawnTabs.empty())
		canvas.Text(kMargin, kMargin + (kTabHeight - line) / 2, L"No program has a dashboard", 0xb0b0b0ff);
	label(QuitRect(), L"Quit game", 0xa53a3aff);
}

namespace {
uint32_t PidOf(const Json::Value& v)
{
	return v.isUInt() ? v.asUInt() : 0;
}
uint64_t StartOf(const Json::Value& v)
{
	return v.isUInt64() ? v.asUInt64() : 0;
}
} // namespace

bool DashboardDocOpen(const Json::Value& doc, const ProcessAlive& alive)
{
	if (!doc.isObject() || !doc["open"].isBool() || !doc["open"].asBool())
		return false;
	const uint32_t scene = PidOf(doc["scene_pid"]);
	return scene && alive(scene, StartOf(doc["scene_start"]));
}

bool DashboardDocRequest(const Json::Value& doc, double nowWall, const ProcessAlive& alive)
{
	if (!doc.isObject() || !doc["open"].isBool() || !doc["open"].asBool() || doc.isMember("scene_pid"))
		return false;
	const uint32_t by = PidOf(doc["requested_by"]);
	const double at = doc["requested_at"].isNumeric() ? doc["requested_at"].asDouble() : 0;
	return by && at > 0 && nowWall - at >= 0 && nowWall - at < 2.0 && alive(by, StartOf(doc["requested_start"]));
}

std::optional<std::string> Dashboard::OnClick(float u, float v)
{
	const float x = u * kWidth, y = v * kHeight;
	if (QuitRect().Contains(x, y))
		return std::string("quit");
	for (size_t i = 0; i < drawnTabs.size(); i++) {
		// As Draw stops: a tab with no room isn't drawn
		if (TabRect(i).x + TabRect(i).w > kWidth - kMargin)
			break;
		if (TabRect(i).Contains(x, y))
			return "tab:" + std::to_string(drawnTabs[i].pid) + ":" + std::to_string(drawnTabs[i].handle);
	}
	return std::nullopt;
}

Dashboard::Button DashboardButtonFromSetting(const std::string& b)
{
	if (b == "menu_long" || b == "system_long")
		return Dashboard::Button::MenuLong;
	if (b == "system")
		return Dashboard::Button::PressOrHold;
	return b == "menu" ? Dashboard::Button::Menu : Dashboard::Button::Off;
}

bool SceneDocHasDashboard(const Json::Value& scene, const ProcessAlive& alive)
{
	if (!scene.isObject() || !scene["dashboard"].isBool())
		return true;
	const uint32_t pid = PidOf(scene["pid"]);
	if (!pid || !alive(pid, StartOf(scene["start"])))
		return true;
	return scene["dashboard"].asBool();
}

std::vector<std::string> StartAutolaunchOverlays(BaseApplications& apps)
{
	std::vector<std::string> tried;
	const uint32_t count = apps.GetApplicationCount();
	for (uint32_t i = 0; i < count; i++) {
		char key[256];
		if (apps.GetApplicationKeyByIndex(i, key, sizeof(key)) != VRApplicationError_None || !apps.GetApplicationAutoLaunch(key))
			continue;
		BaseApplications::EVRApplicationError error = VRApplicationError_None;
		if (!apps.GetApplicationPropertyBool(key, VRApplicationProperty_IsDashboardOverlay_Bool, &error) || error != VRApplicationError_None)
			continue;
		apps.LaunchDashboardOverlay(key);
		tried.push_back(key);
	}
	return tried;
}
