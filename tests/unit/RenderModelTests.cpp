// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

#include "Drivers/Backend.h"
#include "Misc/BuiltInResource.h"
#include "Misc/SteamVRRenderModels.h"
#include "Reimpl/BaseRenderModels.h"
#include "generated/static_bases.gen.h"
#include "resources.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

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

TEST_CASE("SteamVR_models_follow_the_test_folder_whatever_was_asked_before")
{
	// Asked first with no SteamVR folder named (the answer then is the user's install, if steamvrRenderModels is on)
	(void)FindSteamVRRenderModel("test_controller");
	(void)SteamVRRenderModelNames();
	{
		SteamVRDirForTest dir(FixtureSteamVR());
		CHECK(FindSteamVRRenderModel("test_controller") != nullptr);
		std::vector<std::string> names = SteamVRRenderModelNames();
		CHECK(std::find(names.begin(), names.end(), "test_controller") != names.end());
	}
	// Another folder: its models, not the first one's
	TempDir other;
	fs::create_directories(other.path / "resources" / "rendermodels");
	SteamVRDirForTest dir(other.path);
	CHECK(FindSteamVRRenderModel("test_controller") == nullptr);
	CHECK(SteamVRRenderModelNames().empty());
}

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

namespace {
Image MakeImage(uint32_t w, uint32_t h, std::initializer_list<uint8_t> firstPixels = {})
{
	Image image;
	image.width = w;
	image.height = h;
	image.rgba.assign((size_t)w * h * 4, 0);
	size_t i = 0;
	for (uint8_t v : firstPixels)
		image.rgba[i++] = v;
	return image;
}
} // namespace

TEST_CASE("Textures_resample_to_the_target_size")
{
	// A 2x2 of four colours, one per quadrant, grown to 4x4: each quadrant is 2x2 pixels of its colour
	Image src = MakeImage(2, 2, { 1, 1, 1, 255, 2, 2, 2, 255, 3, 3, 3, 255, 4, 4, 4, 255 });
	Image big = ResampleImage(src, 4, 4);
	REQUIRE(big.width == 4);
	REQUIRE(big.height == 4);
	CHECK(big.rgba.size() == 64);
	CHECK(big.At(0, 0) == src.At(0, 0));
	CHECK(big.At(1, 1) == src.At(0, 0));
	CHECK(big.At(2, 1) == src.At(1, 0));
	CHECK(big.At(1, 3) == src.At(0, 1));
	CHECK(big.At(3, 3) == src.At(1, 1));

	// The same size is a copy; a size of 0 or an empty image gives nothing
	CHECK(ResampleImage(src, 2, 2).rgba == src.rgba);
	CHECK(ResampleImage(src, 0, 4).Empty());
	CHECK(ResampleImage(Image{}, 4, 4).Empty());

	// Smaller than the original, down to 1x1
	Image one = ResampleImage(big, 1, 1);
	REQUIRE(one.width == 1);
	CHECK(one.rgba.size() == 4);
}

TEST_CASE("Texture_mip_chain_halves_down_to_one_pixel")
{
	Image src = MakeImage(4, 2);
	for (size_t i = 0; i < src.rgba.size(); i++)
		src.rgba[i] = (uint8_t)(i % 4 == 3 ? 255 : 100);

	std::vector<Image> all = BuildMipChain(src, 0);
	REQUIRE(all.size() == 3);
	CHECK(all[0].width == 4);
	CHECK(all[1].width == 2);
	CHECK(all[1].height == 1);
	CHECK(all[2].width == 1);
	CHECK(all[2].height == 1);
	// Pixels of one colour stay that colour
	CHECK(all[2].At(0, 0) == 0x646464ffu);

	// A fixed number of levels, never more than there are
	CHECK(BuildMipChain(src, 2).size() == 2);
	CHECK(BuildMipChain(src, 20).size() == 3);
	CHECK(BuildMipChain(src, 1).size() == 1);

	// The average of four
	Image four = MakeImage(2, 2, { 0, 0, 0, 255, 100, 0, 0, 255, 200, 0, 0, 255, 255, 0, 0, 255 });
	CHECK(BuildMipChain(four, 0).back().rgba[0] == 139);
	// An empty image is still one level
	CHECK(BuildMipChain(Image{}, 0).size() == 1);
}

namespace {
// A TGA file's bytes: its 18-byte header, then the pixels as given
std::string Tga(uint8_t type, uint16_t width, uint16_t height, uint8_t bits, uint8_t descriptor, const std::string& pixels)
{
	std::string h(18, '\0');
	h[2] = (char)type;
	h[12] = (char)(width & 0xff);
	h[13] = (char)(width >> 8);
	h[14] = (char)(height & 0xff);
	h[15] = (char)(height >> 8);
	h[16] = (char)bits;
	h[17] = (char)descriptor;
	return h + pixels;
}

std::string Pixel(const Image& image, uint32_t x, uint32_t y)
{
	char hex[9];
	const uint8_t* p = &image.rgba[((size_t)y * image.width + x) * 4];
	std::snprintf(hex, sizeof(hex), "%02x%02x%02x%02x", p[0], p[1], p[2], p[3]);
	return hex;
}
} // namespace

TEST_CASE("Render_model_textures_can_be_TGA")
{
	TempDir tmp;
	// Plain true colour, 24 bits (BGR), its rows from the bottom: the first row given is the image's last
	WriteFile(tmp.path / "plain.tga", Tga(2, 2, 2, 24, 0, std::string("\x00\x00\xff" "\x00\xff\x00" "\xff\x00\x00" "\x10\x20\x30", 12)));
	std::optional<Image> plain = LoadRenderModelTexture(tmp.path / "plain.tga");
	REQUIRE(plain.has_value());
	CHECK(plain->width == 2);
	CHECK(plain->height == 2);
	CHECK(Pixel(*plain, 0, 0) == "0000ffff");
	CHECK(Pixel(*plain, 1, 0) == "302010ff");
	CHECK(Pixel(*plain, 0, 1) == "ff0000ff");
	CHECK(Pixel(*plain, 1, 1) == "00ff00ff");

	// Run-length encoded, 32 bits from the top: a run of three, then one pixel on its own
	WriteFile(tmp.path / "rle.tga", Tga(10, 4, 1, 32, 0x20, std::string("\x82" "\x01\x02\x03\x80" "\x00" "\x04\x05\x06\x07", 10)));
	std::optional<Image> rle = LoadRenderModelTexture(tmp.path / "rle.tga");
	REQUIRE(rle.has_value());
	CHECK(Pixel(*rle, 0, 0) == "03020180");
	CHECK(Pixel(*rle, 2, 0) == "03020180");
	CHECK(Pixel(*rle, 3, 0) == "06050407");

	// Greyscale
	WriteFile(tmp.path / "grey.tga", Tga(3, 1, 1, 8, 0x20, std::string("\x7f", 1)));
	std::optional<Image> grey = LoadRenderModelTexture(tmp.path / "grey.tga");
	REQUIRE(grey.has_value());
	CHECK(Pixel(*grey, 0, 0) == "7f7f7fff");

	// Cut short, colour-mapped, or 16 bits: none
	WriteFile(tmp.path / "short.tga", Tga(2, 2, 2, 24, 0, std::string("\x00\x00\xff", 3)));
	CHECK_FALSE(LoadRenderModelTexture(tmp.path / "short.tga").has_value());
	WriteFile(tmp.path / "mapped.tga", Tga(1, 1, 1, 8, 0, std::string("\x00", 1)));
	CHECK_FALSE(LoadRenderModelTexture(tmp.path / "mapped.tga").has_value());
	WriteFile(tmp.path / "sixteen.tga", Tga(2, 1, 1, 16, 0, std::string("\x00\x00", 2)));
	CHECK_FALSE(LoadRenderModelTexture(tmp.path / "sixteen.tga").has_value());
	// A header that claims the largest size with no pixels after it, plain or run-length encoded
	WriteFile(tmp.path / "empty.tga", Tga(2, 8192, 8192, 32, 0, ""));
	CHECK_FALSE(LoadRenderModelTexture(tmp.path / "empty.tga").has_value());
	WriteFile(tmp.path / "empty_rle.tga", Tga(10, 8192, 8192, 32, 0, std::string("\xff\x00\x00\x00\x00", 5)));
	CHECK_FALSE(LoadRenderModelTexture(tmp.path / "empty_rle.tga").has_value());
}

TEST_CASE("Render_model_with_several_materials_gets_their_atlas")
{
	TempDir tmp;
	fs::path dir = tmp.path / "two";
	// Two textures: red 2x2 and blue 1x1 (TGA, from the top)
	WriteFile(dir / "red.tga", Tga(2, 2, 2, 24, 0x20, std::string("\x00\x00\xff\x00\x00\xff\x00\x00\xff\x00\x00\xff", 12)));
	WriteFile(dir / "blue.tga", Tga(2, 1, 1, 24, 0x20, std::string("\xff\x00\x00", 3)));
	WriteFile(dir / "two.mtl", "newmtl red\nmap_Kd red.tga\nnewmtl blue\nmap_Kd blue.tga\n");
	// Two triangles sharing a corner, one of each material
	WriteFile(dir / "two.obj", "mtllib two.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nv 1 1 0\nvt 0 0\nvt 1 1\n"
	                           "usemtl red\nf 1/1 2/2 3/1\nusemtl blue\nf 2/2 4/1 3/1\n");
	std::optional<RmMesh> mesh = LoadObjMesh(dir / "two.obj");
	REQUIRE(mesh.has_value());
	REQUIRE(mesh->textures.size() == 2);
	CHECK(mesh->textures[0].filename() == "red.tga");
	CHECK(mesh->textures[1].filename() == "blue.tga");
	CHECK(mesh->diffuse == mesh->textures[0]);
	// The shared corners are a vertex for each material; each material's v in its half of the atlas
	CHECK(mesh->vertices.size() == 6);
	REQUIRE(mesh->indices.size() == 6);
	for (size_t i = 0; i < 3; i++) {
		float v = mesh->vertices[mesh->indices[i]].rfTextureCoord[1];
		CHECK(v >= 0.0f);
		CHECK(v <= 0.5f);
	}
	for (size_t i = 3; i < 6; i++) {
		float v = mesh->vertices[mesh->indices[i]].rfTextureCoord[1];
		CHECK(v >= 0.5f);
		CHECK(v <= 1.0f);
	}

	// The atlas: both at the widest's size, red above blue
	std::optional<Image> atlas = LoadRenderModelTextures(mesh->textures);
	REQUIRE(atlas.has_value());
	CHECK(atlas->width == 2);
	CHECK(atlas->height == 4);
	CHECK(Pixel(*atlas, 1, 1) == "ff0000ff");
	CHECK(Pixel(*atlas, 0, 2) == "0000ffff");
	CHECK(Pixel(*atlas, 1, 3) == "0000ffff");

	// One material: its texture as it is, the UVs untouched; LoadObj tells the first material's texture
	WriteFile(dir / "one.obj", "mtllib two.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 1\nusemtl blue\nf 1/1 2/2 3/1\n");
	std::optional<RmMesh> one = LoadObjMesh(dir / "one.obj");
	REQUIRE(one.has_value());
	CHECK(one->textures.size() == 1);
	CHECK(one->vertices[0].rfTextureCoord[1] == 1.0f);
	std::vector<RmVertex> verts;
	std::vector<uint16_t> indices;
	std::string diffuse;
	CHECK(LoadObj(dir / "two.obj", verts, indices, &diffuse));
	CHECK(diffuse == "red.tga");
}

TEST_CASE("Render_model_atlas_refuses_textures_too_big_together")
{
	TempDir tmp;
	// Two greyscale images of 8192x4097, run-length encoded: small files, each just over half the largest image
	const size_t pixels = (size_t)8192 * 4097;
	std::string runs;
	for (size_t done = 0; done < pixels; done += 128)
		runs += std::string("\xff\x40", 2);
	WriteFile(tmp.path / "a.tga", Tga(11, 8192, 4097, 8, 0x20, runs));
	WriteFile(tmp.path / "b.tga", Tga(11, 8192, 4097, 8, 0x20, runs));
	REQUIRE(LoadRenderModelTexture(tmp.path / "a.tga").has_value());
	CHECK_FALSE(LoadRenderModelTextures({ tmp.path / "a.tga", tmp.path / "b.tga" }).has_value());
}

TEST_CASE("Built_in_render_models_are_read_by_the_OBJ_reader")
{
	for (int id : { RES_O_HAND_LEFT, RES_O_HAND_RIGHT, RES_O_VIVE_TRACKER }) {
		std::vector<char> text = ReadBuiltInResource(id, RES_T_OBJ);
		REQUIRE_FALSE(text.empty());
		std::vector<RmVertex> verts;
		std::vector<uint16_t> indices;
		REQUIRE(ParseObj(std::string_view(text.data(), text.size()), verts, indices, nullptr));
		CHECK(indices.size() % 3 == 0);
		CHECK(verts.size() <= indices.size());
		// A corner the faces share is one vertex: the tracker's have
		if (id == RES_O_VIVE_TRACKER)
			CHECK(verts.size() < indices.size());
	}
}

TEST_CASE("Render_models_load_asynchronously")
{
	BaseRenderModels rm;
	OOVR_RenderModel_t* model = nullptr;
	// The first call starts the load; it's answered once done, and at once after that
	CHECK((int)rm.LoadRenderModel_Async("renderLeftHand", &model) == 100);
	CHECK(model == nullptr);
	BaseRenderModels::WaitForLoads();
	CHECK((int)rm.LoadRenderModel_Async("renderLeftHand", &model) == 0);
	REQUIRE(model != nullptr);
	rm.FreeRenderModel(model);
	model = nullptr;
	CHECK((int)rm.LoadRenderModel_Async("renderLeftHand", &model) == 0);
	rm.FreeRenderModel(model);

	// Waited for: the model in one call
	model = nullptr;
	CHECK((int)rm.LoadRenderModelNow("renderRightHand", &model) == 0);
	REQUIRE(model != nullptr);
	rm.FreeRenderModel(model);

	// A name with no model: loading, then not one
	model = nullptr;
	CHECK((int)rm.LoadRenderModel_Async("no_such_model_here", &model) == 100);
	BaseRenderModels::WaitForLoads();
	CHECK((int)rm.LoadRenderModel_Async("no_such_model_here", &model) == 301);
	CHECK(model == nullptr);
}

// OpenLamina's own controller models, as the build converts them (scripts/rendermodels.py)
namespace {
struct OwnModelsDirForTest {
	explicit OwnModelsDirForTest(const fs::path& dir)
	{
#ifdef _WIN32
		_wputenv_s(L"OC_TEST_RENDERMODELS_DIR", dir.wstring().c_str());
#else
		setenv("OC_TEST_RENDERMODELS_DIR", dir.c_str(), 1);
#endif
	}
	~OwnModelsDirForTest()
	{
#ifdef _WIN32
		_wputenv_s(L"OC_TEST_RENDERMODELS_DIR", L"");
#else
		unsetenv("OC_TEST_RENDERMODELS_DIR");
#endif
	}
};

fs::path OwnModels()
{
	return fs::path(OC_TEST_RENDERMODELS);
}

glm::vec3 Apply(const XrPosef& p, glm::vec3 v)
{
	glm::quat q(p.orientation.w, p.orientation.x, p.orientation.y, p.orientation.z);
	return q * v + glm::vec3(p.position.x, p.position.y, p.position.z);
}

glm::vec3 Pos(const RmVertex& v)
{
	return glm::vec3(v.vPosition.v[0], v.vPosition.v[1], v.vPosition.v[2]);
}
} // namespace

TEST_CASE("Own_controller_models_are_in_SteamVR_layout")
{
	OwnModelsDirForTest dir(OwnModels());
	const std::vector<std::string>& names = OwnRenderModelNames();
	// Twelve controllers, both hands
	REQUIRE(names.size() == 24);
	CHECK(std::is_sorted(names.begin(), names.end()));
	for (const std::string& name : names) {
		CAPTURE(name);
		std::shared_ptr<const RmModel> model = FindOwnRenderModel(name, XrPosef{ { 0, 0, 0, 1 }, { 0, 0, 0 } });
		REQUIRE(model);
		CHECK(Find(*model, "body"));
		CHECK(Find(*model, "trigger"));
		// No tip, base or handgrip: those are the profile's, as for any controller (GetComponentState)
		CHECK_FALSE(Find(*model, "tip"));
		CHECK_FALSE(Find(*model, "base"));
		CHECK_FALSE(Find(*model, "handgrip"));
		for (const RmComponent& c : model->components) {
			CAPTURE(c.name);
			std::optional<RmMesh> mesh = LoadRenderModelMesh(*model, c.name);
			REQUIRE(mesh);
			CHECK(!mesh->textures.empty());
			CHECK(LoadRenderModelTextures(mesh->textures));
		}
		// The whole model: the components shown at rest
		std::optional<RmMesh> whole = LoadRenderModelMesh(*model, "");
		REQUIRE(whole);
		CHECK(whole->indices.size() % 3 == 0);
	}
}

TEST_CASE("Own_controller_models_move_like_SteamVR_ones")
{
	OwnModelsDirForTest dir(OwnModels());
	std::shared_ptr<const RmModel> index = FindOwnRenderModel("valve-index-left", XrPosef{ { 0, 0, 0, 1 }, { 0, 0, 0 } });
	REQUIRE(index);

	// The trigger turns on the trigger's axis, the grip with its button; the stick with axis 0
	const RmComponent* trigger = Find(*index, "trigger");
	REQUIRE(trigger);
	CHECK(trigger->motionType == "rotate");
	CHECK(trigger->motion["controller_axis"].asInt() == 1);
	const RmComponent* stick = Find(*index, "thumbstick");
	REQUIRE(stick);
	CHECK(stick->motionType == "joystick");
	CHECK(stick->motion["controller_axis"].asInt() == 0);

	// The touch dot: hidden until the trackpad's touched
	const RmComponent* dot = Find(*index, "trackpad_touch");
	REQUIRE(dot);
	CHECK(dot->motionType == "trackpad");
	CHECK_FALSE(ComponentVisible(*dot, false, false));
	CHECK(ComponentVisible(*dot, true, false));
	// Touched at the pad's right, it's further right than at its left
	XrPosef right = ComponentPose(*dot, 0, { { 1, 0 } }), left = ComponentPose(*dot, 0, { { -1, 0 } });
	glm::vec3 across(right.position.x - left.position.x, right.position.y - left.position.y, right.position.z - left.position.z);
	CHECK(glm::length(across) > 0.005f);
}

TEST_CASE("Own_controller_models_are_put_in_the_device_space")
{
	OwnModelsDirForTest dir(OwnModels());
	// A space of its own, so the moved model isn't the cached one at identity
	TempDir copy("oc_own_models");
	fs::copy(OwnModels() / "valve-index", copy.path / "valve-index", fs::copy_options::recursive);
	std::optional<RmModel> files = LoadRenderModelFolder(copy.path / "valve-index", "valve-index-left");
	REQUIRE(files);
	RmModel moved = *files;
	glm::quat q = glm::angleAxis(glm::radians(30.0f), glm::normalize(glm::vec3(1, 0.5f, 0)));
	moved.space = XrPosef{ { q.x, q.y, q.z, q.w }, { 0.01f, -0.02f, 0.03f } };

	// The vertices: each where the space puts it
	std::optional<RmMesh> a = LoadRenderModelMesh(*files, "trigger"), b = LoadRenderModelMesh(moved, "trigger");
	REQUIRE(a);
	REQUIRE(b);
	REQUIRE(a->vertices.size() == b->vertices.size());
	for (size_t i = 0; i < a->vertices.size(); i += 17)
		CHECK(glm::length(Apply(moved.space, Pos(a->vertices[i])) - Pos(b->vertices[i])) < 1e-5f);

	// The motion with them: the pressed trigger's vertex in the device's space is the files' pressed one, moved
	const RmComponent* trigger = Find(moved, "trigger");
	REQUIRE(trigger);
	XrPosef inFiles = ComponentPose(*trigger, 1, { { 0, 0 } });
	XrPosef inDevice = ModelComponentPose(moved, *trigger, 1, { { 0, 0 } });
	for (size_t i = 0; i < a->vertices.size(); i += 17) {
		glm::vec3 want = Apply(moved.space, Apply(inFiles, Pos(a->vertices[i])));
		glm::vec3 got = Apply(inDevice, Pos(b->vertices[i]));
		CHECK(glm::length(want - got) < 1e-5f);
	}
	// At rest it doesn't move
	XrPosef rest = ModelComponentPose(moved, *trigger, 0, { { 0, 0 } });
	CHECK(std::abs(rest.position.x) + std::abs(rest.position.y) + std::abs(rest.position.z) < 1e-5f);
}

TEST_CASE("Own_controller_models_textures_can_be_JPEG")
{
	OwnModelsDirForTest dir(OwnModels());
	// The PICO Neo3's texture is a JPEG
	std::shared_ptr<const RmModel> pico = FindOwnRenderModel("pico-neo3-left", XrPosef{ { 0, 0, 0, 1 }, { 0, 0, 0 } });
	REQUIRE(pico);
	std::optional<RmMesh> mesh = LoadRenderModelMesh(*pico, "body");
	REQUIRE(mesh);
	REQUIRE(mesh->textures.size() == 1);
	CHECK(mesh->textures[0].extension() == ".jpg");
	std::optional<Image> image = LoadRenderModelTexture(mesh->textures[0]);
	REQUIRE(image);
	CHECK(image->width > 0);
	CHECK(image->rgba.size() == (size_t)image->width * image->height * 4);
	// Not all one colour
	bool varied = false;
	for (size_t i = 4; i < image->rgba.size() && !varied; i += 4)
		varied = memcmp(&image->rgba[i], &image->rgba[0], 3) != 0;
	CHECK(varied);
	// The same through the overlay's and the compositor's reader
	std::optional<Image> again = LoadImageFile(mesh->textures[0], nullptr);
	REQUIRE(again);
	CHECK(again->rgba == image->rgba);

	// Bytes that start as a JPEG but aren't one are refused
	TempDir tmp;
	WriteFile(tmp.path / "broken.jpg", std::string("\xFF\xD8\xFF\xE0 not really", 15));
	CHECK_FALSE(LoadRenderModelTexture(tmp.path / "broken.jpg"));
}

TEST_CASE("Own_controller_models_answer_through_IVRRenderModels")
{
	OwnModelsDirForTest dir(OwnModels());
	// SteamVR's own models (this machine's install) left out: the fixture has none of these names
	SteamVRDirForTest steamvr(FixtureSteamVR());
	BaseRenderModels rm;
	const char* vive = "htc-vive-right";
	CHECK(rm.GetComponentCount(vive) == 5); // body, grip, trackpad, trackpad_touch, trigger
	CHECK(rm.RenderModelHasComponent(vive, "trackpad_touch"));
	CHECK_FALSE(rm.RenderModelHasComponent(vive, "tip"));
	char name[128];
	CHECK(rm.GetComponentRenderModelName(vive, "trigger", name, sizeof(name)) == strlen("htc-vive-right/trigger") + 1);
	CHECK(std::string(name) == "htc-vive-right/trigger");
	CHECK(rm.GetComponentButtonMask(vive, "trigger") == vr::ButtonMaskFromId(vr::k_EButton_Axis1));
	CHECK(rm.GetComponentButtonMask(vive, "grip") == vr::ButtonMaskFromId(vr::k_EButton_Grip));

	// The touch dot shows while the pad's touched
	vr::VRControllerState_t state{};
	OOVR_RenderModel_ComponentState_t cs{};
	REQUIRE(rm.GetComponentState(vive, "trackpad_touch", &state, nullptr, &cs));
	CHECK((cs.uProperties & 2) == 0); // VRComponentProperty_IsVisible
	state.ulButtonTouched = vr::ButtonMaskFromId(vr::k_EButton_Axis0);
	REQUIRE(rm.GetComponentState(vive, "trackpad_touch", &state, nullptr, &cs));
	CHECK((cs.uProperties & 2) != 0);
	// Its tip is answered without a controller: where the model is
	CHECK(rm.GetComponentState(vive, "tip", &state, nullptr, &cs));

	// The model and a component load, with a texture
	vr::IVRRenderModels_001::RenderModel_t whole{}, part{};
	REQUIRE(rm.LoadRenderModelInline(vive, &whole));
	REQUIRE(rm.LoadRenderModelInline("htc-vive-right/trigger", &part));
	CHECK(whole.unTriangleCount > part.unTriangleCount);
	CHECK(whole.diffuseTexture.unWidth == 2048);
	rm.FreeRenderModelInline(&whole);
	rm.FreeRenderModelInline(&part);

	// The Touch's name, with no controller in the hand: the Touch's model
	CHECK(rm.GetComponentCount("oculus_quest2_controller_left") > 1);
	CHECK(rm.RenderModelHasComponent("oculus_quest2_controller_left", "x_button"));
	// A hand's own model with no controller in it stays the hand
	CHECK(rm.GetComponentCount("renderLeftHand") == 1);
}

namespace {

// Both hands are there, and no input has been made yet: a game setting up its models before its first frame
class HandDevice : public ITrackedDevice {
public:
	void GetPose(vr::ETrackingUniverseOrigin, vr::TrackedDevicePose_t* pose, ETrackingStateType, int64_t) override { *pose = {}; }
	vr::ETrackedDeviceClass GetTrackedDeviceClass() override { return vr::TrackedDeviceClass_Controller; }
	vr::ETrackedControllerRole GetControllerRole() override { return vr::TrackedControllerRole_LeftHand; }
};

class HandsBackend : public IBackend {
public:
	std::shared_ptr<HandDevice> hand = std::make_shared<HandDevice>();
	int otherHandsAsked = 0;
	uint32_t CurrentFrameId() override { return 0; }
	float SecondsToFrame(uint32_t) override { return NAN; }
	std::shared_ptr<IHMD> GetPrimaryHMD() override { return nullptr; }
	std::shared_ptr<ITrackedDevice> GetDevice(vr::TrackedDeviceIndex_t) override { return nullptr; }
	std::shared_ptr<ITrackedDevice> GetDeviceByHand(ITrackedDevice::TrackedDeviceType type) override
	{
		if (type == ITrackedDevice::HAND_LEFT || type == ITrackedDevice::HAND_RIGHT)
			return hand;
		otherHandsAsked++;
		return nullptr;
	}
	void GetDeviceToAbsoluteTrackingPose(vr::ETrackingUniverseOrigin, float, vr::TrackedDevicePose_t*, uint32_t) override {}
	void WaitForTrackingData() override {}
	IBackend::openvr_enum_t StoreEyeTexture(vr::EVREye, const vr::Texture_t*, const vr::VRTextureBounds_t*, vr::EVRSubmitFlags, bool) override { return 0; }
	void SubmitFrames(bool) override {}
	void ClearSubmittedFrame() override {}
	void SetRenderingSuspended(bool) override {}
	IBackend::openvr_enum_t SetSkyboxOverride(const vr::Texture_t*, uint32_t) override { return 0; }
	void ClearSkyboxOverride() override {}
	bool GetFrameTiming(OOVR_Compositor_FrameTiming*, uint32_t) override { return false; }
	IBackend::openvr_enum_t GetMirrorTextureD3D11(vr::EVREye, void*, void**) override { return 0; }
	void ReleaseMirrorTextureD3D11(void*) override {}
	bool GetPlayAreaPoints(vr::HmdVector3_t*, int*) override { return false; }
	bool AreBoundsVisible() override { return false; }
	void ForceBoundsVisible(bool) override {}
	void PumpEvents() override {}
	bool IsInputAvailable() override { return false; }
	bool IsInputFocusLost() override { return false; }
	bool ShouldReduceRenderingWork() override { return false; }
	bool IsGraphicsConfigured() override { return false; }
	void OnOverlayTexture(const vr::Texture_t*) override {}
};

struct WithHands {
	HandsBackend* backend = new HandsBackend();
	WithHands() { BackendManager::Create(backend); }
	~WithHands() { BackendManager::Reset(); }
};

}

TEST_CASE("Hand_model_tip_is_answered_before_input_is_made")
{
	REQUIRE(GetBaseInput() == nullptr);
	WithHands hands;
	BaseRenderModels rm;
	vr::VRControllerState_t state{};
	OOVR_RenderModel_ComponentState_t cs{};
	// No input to ask where the aim is: the model's own place
	CHECK(rm.GetComponentState("renderLeftHand", "tip", &state, nullptr, &cs));

	// A tracker isn't a hand: the backend isn't asked for one
	rm.GetComponentState("vive_tracker", "tip", &state, nullptr, &cs);
	CHECK(hands.backend->otherHandsAsked == 0);
}
