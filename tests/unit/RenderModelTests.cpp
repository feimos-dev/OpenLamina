// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

#include "Misc/SteamVRRenderModels.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

namespace {

// The fixture: a SteamVR folder of our own with resources/rendermodels/test_controller (two components, one PNG)
// and vive_tracker (a JSON file that isn't JSON)
fs::path FixtureSteamVR()
{
	return fs::path(OC_TEST_DATA_DIR) / "steamvr";
}

// A fresh folder under the system's temporary one, removed again at the end
struct TempDir {
	fs::path path;
	explicit TempDir(const fs::path& name = "oc_rendermodels")
	{
		auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		path = fs::temp_directory_path() / (name.u8string() + u8"_" + fs::path(std::to_string(stamp)).u8string());
		fs::create_directories(path);
	}
	~TempDir()
	{
		std::error_code ec;
		fs::remove_all(path, ec);
	}
};

void WriteFile(const fs::path& p, const std::string& text)
{
	fs::create_directories(p.parent_path());
	std::ofstream(p, std::ios::binary) << text;
}

// OC_TEST_STEAMVR_DIR set for one test: FindSteamVRInstall() answers that folder
struct SteamVRDirForTest {
	explicit SteamVRDirForTest(const fs::path& dir)
	{
#ifdef _WIN32
		_wputenv_s(L"OC_TEST_STEAMVR_DIR", dir.wstring().c_str());
#else
		setenv("OC_TEST_STEAMVR_DIR", dir.c_str(), 1);
#endif
	}
	~SteamVRDirForTest()
	{
#ifdef _WIN32
		_wputenv_s(L"OC_TEST_STEAMVR_DIR", L"");
#else
		unsetenv("OC_TEST_STEAMVR_DIR");
#endif
	}
};

const RmComponent* Find(const RmModel& m, const std::string& name)
{
	for (const RmComponent& c : m.components)
		if (c.name == name)
			return &c;
	return nullptr;
}

// Where a pose puts a point
XrVector3f Apply(const XrPosef& pose, XrVector3f p)
{
	const XrQuaternionf& q = pose.orientation;
	// v + 2w(u x v) + 2u x (u x v), u = (x, y, z)
	XrVector3f u = { q.x, q.y, q.z };
	auto cross = [](XrVector3f a, XrVector3f b) {
		return XrVector3f{ a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
	};
	XrVector3f t = cross(u, p);
	t = { 2 * t.x, 2 * t.y, 2 * t.z };
	XrVector3f ut = cross(u, t);
	return { p.x + q.w * t.x + ut.x + pose.position.x, p.y + q.w * t.y + ut.y + pose.position.y,
		p.z + q.w * t.z + ut.z + pose.position.z };
}

bool IsIdentity(const XrPosef& p)
{
	return std::fabs(p.orientation.w) > 0.99999f && std::fabs(p.position.x) < 1e-6f && std::fabs(p.position.y) < 1e-6f
	    && std::fabs(p.position.z) < 1e-6f;
}

// The angle a pose turns by about the X axis, in degrees (its orientation must be about X only)
float AngleAboutX(const XrPosef& p)
{
	CHECK(std::fabs(p.orientation.y) < 1e-6f);
	CHECK(std::fabs(p.orientation.z) < 1e-6f);
	return 2 * std::atan2(p.orientation.x, p.orientation.w) * 180.0f / 3.14159265f;
}

} // namespace

TEST_CASE("Render_model_from_folder")
{
	fs::path steamvr = FixtureSteamVR();

	std::vector<std::string> names = ListSteamVRRenderModels(steamvr);
	REQUIRE(names.size() == 2);
	CHECK(names[0] == "test_controller");
	CHECK(names[1] == "vive_tracker");

	std::optional<RmModel> model = LoadSteamVRRenderModel(steamvr, "test_controller");
	REQUIRE(model.has_value());
	CHECK(model->name == "test_controller");
	CHECK(model->thumbnail == "test_controller.png");
	REQUIRE(model->components.size() == 2);
	const RmComponent* body = Find(*model, "body");
	const RmComponent* trigger = Find(*model, "trigger");
	REQUIRE(body);
	REQUIRE(trigger);
	CHECK(body->objFile == "body.obj");
	CHECK(body->motionType.empty());
	CHECK(body->local.position.z == doctest::Approx(0.05f));
	CHECK(trigger->motionType == "rotate");
	// component_local's rotate_xyz: -90 degrees about X
	CHECK(AngleAboutX(trigger->local) == doctest::Approx(-90.0f).epsilon(1e-4));
	CHECK(trigger->local.position.y == doctest::Approx(-0.02f));

	// Each component's OBJ: the plate's four-cornered face is two triangles on four vertices
	std::optional<RmMesh> plate = LoadRenderModelMesh(*model, "body");
	REQUIRE(plate.has_value());
	CHECK(plate->vertices.size() == 4);
	CHECK(plate->indices.size() == 6);
	// UVs start at the top: OBJ's v of 0 is the texture's bottom row
	CHECK(plate->vertices[0].rfTextureCoord[0] == doctest::Approx(0.0f));
	CHECK(plate->vertices[0].rfTextureCoord[1] == doctest::Approx(1.0f));
	CHECK(plate->vertices[2].vPosition.v[2] == doctest::Approx(0.1f));
	CHECK(plate->vertices[0].vNormal.v[1] == doctest::Approx(1.0f));
	std::optional<RmMesh> trig = LoadRenderModelMesh(*model, "trigger");
	REQUIRE(trig.has_value());
	CHECK(trig->vertices.size() == 3);
	CHECK(trig->indices.size() == 3);

	// No test_controller.obj: the whole model is its components together
	std::optional<RmMesh> whole = LoadRenderModelMesh(*model, "");
	REQUIRE(whole.has_value());
	CHECK(whole->vertices.size() == 7);
	CHECK(whole->indices.size() == 9);
	for (uint16_t i : whole->indices)
		CHECK(i < 7);
	REQUIRE(!whole->diffuse.empty());
	CHECK(whole->diffuse.filename() == "test_controller.png");
	// No component of that name
	CHECK(!LoadRenderModelMesh(*model, "no such component").has_value());

	// The texture: 2x2, red, green / blue, white
	std::optional<Image> tex = LoadRenderModelTexture(whole->diffuse);
	REQUIRE(tex.has_value());
	CHECK(tex->width == 2);
	CHECK(tex->height == 2);
	REQUIRE(tex->rgba.size() == 16);
	CHECK(tex->rgba[0] == 255);
	CHECK(tex->rgba[1] == 0);
	CHECK(tex->rgba[3] == 255);
	CHECK(tex->rgba[4 * 2 + 2] == 255); // blue, bottom left

	// A driver's model name gets its own folder; a name that could leave the folder gets none
	CHECK(SteamVRRenderModelFolder(steamvr, "{indexcontroller}knuckles_left")
	    == steamvr / "drivers" / "indexcontroller" / "resources" / "rendermodels" / "knuckles_left");
	CHECK(SteamVRRenderModelFolder(steamvr, "../bin").empty());
	CHECK(SteamVRRenderModelFolder(steamvr, "a/b").empty());
	CHECK(SteamVRRenderModelFolder(steamvr, "{x/..}y").empty());
	CHECK(SteamVRRenderModelFolder(steamvr, "{unclosed").empty());
	CHECK(SteamVRRenderModelFolder(steamvr, "").empty());
}

TEST_CASE("Trigger_component_rotates_with_value")
{
	std::optional<RmModel> model = LoadSteamVRRenderModel(FixtureSteamVR(), "test_controller");
	REQUIRE(model.has_value());
	const RmComponent* trigger = Find(*model, "trigger");
	REQUIRE(trigger);
	XrVector3f pivot = { 0.0f, -0.01f, 0.02f };

	// At rest: where the OBJ has it
	CHECK(IsIdentity(ComponentPose(*trigger, 0.0f, { 0, 0 })));

	// Pulled all the way: value_mapping's -20 degrees about X, through the pivot
	XrPosef full = ComponentPose(*trigger, 1.0f, { 0, 0 });
	CHECK(AngleAboutX(full) == doctest::Approx(-20.0f).epsilon(1e-4));
	XrVector3f p = Apply(full, pivot);
	CHECK(p.x == doctest::Approx(pivot.x));
	CHECK(p.y == doctest::Approx(pivot.y));
	CHECK(p.z == doctest::Approx(pivot.z));
	// A point off the pivot moves
	XrVector3f tip = Apply(full, { 0.0f, -0.03f, 0.02f });
	CHECK(std::fabs(tip.z - 0.02f) > 0.005f);

	// Half way: half the angle
	CHECK(AngleAboutX(ComponentPose(*trigger, 0.5f, { 0, 0 })) == doctest::Approx(-10.0f).epsilon(1e-4));

	// The body doesn't move
	const RmComponent* body = Find(*model, "body");
	REQUIRE(body);
	CHECK(IsIdentity(ComponentPose(*body, 1.0f, { 1, 1 })));

	// A button moves along its axis by value_mapping's metres
	RmComponent button;
	button.motionType = "translate";
	Json::Value motion;
	motion["type"] = "translate";
	motion["value_mapping"].append(0.0);
	motion["value_mapping"].append(0.002);
	motion["axis"].append(0.0);
	motion["axis"].append(-1.0);
	motion["axis"].append(0.0);
	button.motion = motion;
	XrPosef pressed = ComponentPose(button, 1.0f, { 0, 0 });
	CHECK(pressed.position.y == doctest::Approx(-0.002f));
	CHECK(IsIdentity(ComponentPose(button, 0.0f, { 0, 0 })));

	// A joystick pushed right tilts its top to the right, about its centre
	RmComponent stick;
	stick.motionType = "joystick";
	Json::Value js;
	js["type"] = "joystick";
	for (float v : { 0.0f, 0.01f, 0.05f })
		js["center"].append(v);
	js["joystick_rotation_x"].append(20.0);
	js["joystick_rotation_x"].append(-20.0);
	js["joystick_rotation_y"].append(20.0);
	js["joystick_rotation_y"].append(-20.0);
	stick.motion = js;
	CHECK(IsIdentity(ComponentPose(stick, 0.0f, { 0, 0 })));
	XrPosef right = ComponentPose(stick, 0.0f, { 1, 0 });
	XrVector3f top = Apply(right, { 0.0f, 0.03f, 0.05f });
	CHECK(top.x > 0.005f);
	XrVector3f centre = Apply(right, { 0.0f, 0.01f, 0.05f });
	CHECK(centre.x == doctest::Approx(0.0f));
	CHECK(centre.y == doctest::Approx(0.01f));
	// Pushed forward (y up), its top goes forward: towards -Z
	XrVector3f fwd = Apply(ComponentPose(stick, 0.0f, { 0, 1 }), { 0.0f, 0.03f, 0.05f });
	CHECK(fwd.z < 0.045f);

	// Visibility: default, and touch/press entries while touched or pressed
	RmComponent dot;
	Json::Value vis;
	vis["default"] = false;
	vis["touch"] = true;
	dot.visibility = vis;
	CHECK(!ComponentVisible(dot, false, false));
	CHECK(ComponentVisible(dot, true, false));
	CHECK(ComponentVisible(*body, false, false));
}

TEST_CASE("Render_model_garbage_falls_back")
{
	// The fixture's vive_tracker.json isn't JSON: no model
	CHECK(!LoadSteamVRRenderModel(FixtureSteamVR(), "vive_tracker").has_value());

	TempDir tmp;
	fs::path rm = tmp.path / "resources" / "rendermodels";
	fs::path obj = fs::path(OC_TEST_DATA_DIR) / "steamvr" / "resources" / "rendermodels" / "test_controller" / "body.obj";
	// JSON that isn't a model's, beside a good OBJ: still no model (the JSON is broken, not missing)
	WriteFile(rm / "array" / "array.json", "[1, 2, 3]");
	WriteFile(rm / "number" / "number.json", "{ \"components\": 5 }");
	WriteFile(rm / "component" / "component.json", "{ \"components\": { \"body\": \"body.obj\" } }");
	WriteFile(rm / "local" / "local.json", "{ \"components\": { \"body\": { \"component_local\": { \"origin\": \"here\" } } } }");
	WriteFile(rm / "empty" / "empty.json", "");
	WriteFile(rm / "binary" / "binary.json", std::string("\x89PNG\r\n\x1a\n\0\0\0", 11));
	for (const char* name : { "array", "number", "component", "local", "empty", "binary" }) {
		fs::copy_file(obj, rm / name / (std::string(name) + ".obj"));
		CHECK_MESSAGE(!LoadSteamVRRenderModel(tmp.path, name).has_value(), name);
	}
	// Neither JSON nor OBJ, no folder at all, or a name that could leave the folder
	fs::create_directories(rm / "nothing");
	CHECK(!LoadSteamVRRenderModel(tmp.path, "nothing").has_value());
	CHECK(!LoadSteamVRRenderModel(tmp.path, "no such model").has_value());
	CHECK(!LoadSteamVRRenderModel(tmp.path, "../resources").has_value());
	// No SteamVR folder at all
	CHECK(ListSteamVRRenderModels(tmp.path / "missing").empty());

	// A model with only its OBJ (most of SteamVR's): no components, its OBJ drawn
	WriteFile(rm / "plain" / "plain.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
	std::optional<RmModel> plain = LoadSteamVRRenderModel(tmp.path, "plain");
	REQUIRE(plain.has_value());
	CHECK(plain->components.empty());
	std::optional<RmMesh> plainMesh = LoadRenderModelMesh(*plain, "");
	REQUIRE(plainMesh.has_value());
	CHECK(plainMesh->vertices.size() == 3);
	CHECK(plainMesh->diffuse.empty());

	// Bad OBJ files: nothing appended
	std::vector<RmVertex> verts;
	std::vector<uint16_t> indices;
	WriteFile(tmp.path / "nowhere.obj", "v 0 0 0\nv 1 0 0\nf 1 2 7\n");
	CHECK(!LoadObj(tmp.path / "nowhere.obj", verts, indices, nullptr));
	WriteFile(tmp.path / "zero.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 0 1 2\n");
	CHECK(!LoadObj(tmp.path / "zero.obj", verts, indices, nullptr));
	WriteFile(tmp.path / "none.obj", "v 0 0 0\n# no faces\n");
	CHECK(!LoadObj(tmp.path / "none.obj", verts, indices, nullptr));
	WriteFile(tmp.path / "junk.obj", "f x/y/z q r\nv nan\n\x01\x02\x03");
	CHECK(!LoadObj(tmp.path / "junk.obj", verts, indices, nullptr));
	CHECK(!LoadObj(tmp.path / "missing.obj", verts, indices, nullptr));
	CHECK(verts.empty());
	CHECK(indices.empty());
	// Negative (relative) corners count back from the last vertex
	WriteFile(tmp.path / "relative.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf -3 -2 -1\n");
	CHECK(LoadObj(tmp.path / "relative.obj", verts, indices, nullptr));
	CHECK(verts.size() == 3);

	// A texture that isn't a PNG, a truncated PNG, and a PNG too large: none
	WriteFile(tmp.path / "tex.tga", std::string("\0\0\x02\0\0\0\0\0", 8));
	CHECK(!LoadRenderModelTexture(tmp.path / "tex.tga").has_value());
	std::ifstream png(fs::path(OC_TEST_DATA_DIR) / "steamvr" / "resources" / "rendermodels" / "test_controller" / "test_controller.png", std::ios::binary);
	std::string bytes((std::istreambuf_iterator<char>(png)), std::istreambuf_iterator<char>());
	REQUIRE(bytes.size() > 40);
	WriteFile(tmp.path / "cut.png", bytes.substr(0, 40));
	CHECK(!LoadRenderModelTexture(tmp.path / "cut.png").has_value());
	std::string huge = bytes;
	// IHDR's width and height (bytes 16-23) as 16384: refused before it's decoded
	huge[16] = 0, huge[17] = 0, huge[18] = 0x40, huge[19] = 0;
	huge[20] = 0, huge[21] = 0, huge[22] = 0x40, huge[23] = 0;
	WriteFile(tmp.path / "huge.png", huge);
	CHECK(!LoadRenderModelTexture(tmp.path / "huge.png").has_value());

	// What BaseRenderModels asks: for the broken model, nothing (it then answers its built-in model); for a good one,
	// the model
	SteamVRDirForTest dir(FixtureSteamVR());
	CHECK(FindSteamVRRenderModel("vive_tracker") == nullptr);
	CHECK(FindSteamVRRenderModel("renderLeftHand") == nullptr);
	std::shared_ptr<const RmModel> good = FindSteamVRRenderModel("test_controller");
	REQUIRE(good != nullptr);
	CHECK(good->components.size() == 2);
}

TEST_CASE("Render_model_non_ascii_folder")
{
	// The SteamVR folder, the model's folder and its files all with names outside ASCII
	TempDir tmp(fs::path(u8"oc_rendermodels_ünïcödé_日本"));
	std::string name = (const char*)u8"modèle_ü";
	fs::path from = FixtureSteamVR() / "resources" / "rendermodels" / "test_controller";
	fs::path to = tmp.path / "resources" / "rendermodels" / fs::path(std::u8string(name.begin(), name.end()));
	fs::create_directories(to);
	for (const char* file : { "body.obj", "trigger.obj", "test_controller.mtl", "test_controller.png" })
		fs::copy_file(from / file, to / file);
	fs::copy_file(from / "test_controller.json", to / fs::path(std::u8string(name.begin(), name.end()) + u8".json"));

	std::vector<std::string> names = ListSteamVRRenderModels(tmp.path);
	REQUIRE(names.size() == 1);
	CHECK(names[0] == name);

	std::optional<RmModel> model = LoadSteamVRRenderModel(tmp.path, name);
	REQUIRE(model.has_value());
	CHECK(model->components.size() == 2);
	std::optional<RmMesh> whole = LoadRenderModelMesh(*model, "");
	REQUIRE(whole.has_value());
	CHECK(whole->vertices.size() == 7);
	std::optional<Image> tex = LoadRenderModelTexture(whole->diffuse);
	REQUIRE(tex.has_value());
	CHECK(tex->width == 2);
}
