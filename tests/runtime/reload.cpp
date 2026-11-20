// What Proton does with vrclient.so: load it, start OpenVR and use it, shut it down, unload it (dlclose), and then do
// it all again in the same process, a few times over; then load it once more and exit without shutting down, as a
// game may. Run with the library's path as the argument and XR_RUNTIME_JSON
// set to the stand-in runtime's manifest (tests/runtime/CMakeLists.txt does both). Exits 0 when every round worked;
// says whether the library left memory on dlclose, since the second load is only a fresh one if it did.
#include <dlfcn.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

// IVRClientCore_003, IVRSystem's first method and IVRCompositor_027's first three: only what's called. Outside the
// anonymous namespace: there the compiler would know no class implements them, and take the calls for unreachable.
struct ClientCore {
	virtual int Init(int appType, const char* startupInfo) = 0;
	virtual void Cleanup() = 0;
	virtual int IsInterfaceVersionValid(const char* version) = 0;
	virtual void* GetGenericInterface(const char* nameAndVersion, int* error) = 0;
};
struct System {
	virtual void GetRecommendedRenderTargetSize(uint32_t* width, uint32_t* height) = 0;
};
struct Compositor {
	virtual void SetTrackingSpace(int origin) = 0;
	virtual int GetTrackingSpace() = 0;
	virtual int WaitGetPoses(void* render, uint32_t renderCount, void* game, uint32_t gameCount) = 0;
};
using Factory = void* (*)(const char* interfaceName, int* returnCode);

namespace {

constexpr int Scene = 1, Background = 3;

bool Round(const char* path, int round, int appType, bool shutdown = true)
{
	void* lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!lib) {
		printf("round %d: dlopen failed: %s\n", round, dlerror());
		return false;
	}
	auto factory = (Factory)dlsym(lib, "VRClientCoreFactory");
	int rc = -1;
	auto* core = factory ? (ClientCore*)factory("IVRClientCore_003", &rc) : nullptr;
	if (!core) {
		printf("round %d: no IVRClientCore_003 (%d)\n", round, rc);
		return false;
	}
	int error = core->Init(appType, nullptr);
	if (error != 0) {
		printf("round %d: Init answered %d\n", round, error);
		return false;
	}
	bool ok = true;
	auto* system = (System*)core->GetGenericInterface("IVRSystem_022", &error);
	if (system && error == 0) {
		uint32_t w = 0, h = 0;
		system->GetRecommendedRenderTargetSize(&w, &h);
		if (w == 0 || h == 0) {
			printf("round %d: render target size %ux%u\n", round, w, h);
			ok = false;
		}
	} else {
		printf("round %d: no IVRSystem_022 (%d)\n", round, error);
		ok = false;
	}
	if (appType == Scene) {
		auto* compositor = (Compositor*)core->GetGenericInterface("IVRCompositor_027", &error);
		if (compositor && error == 0) {
			compositor->SetTrackingSpace(1); // standing
			for (int frame = 0; frame < 3; frame++)
				compositor->WaitGetPoses(nullptr, 0, nullptr, 0);
		} else {
			printf("round %d: no IVRCompositor_027 (%d)\n", round, error);
			ok = false;
		}
	}
	if (!shutdown) {
		// The game exits without shutting OpenVR down: OpenLamina's static destructors end the session, and they run
		// with the bundled OpenXR loader's
		printf("round %d: exiting without Cleanup\n", round);
		return ok;
	}
	core->Cleanup();
	if (dlclose(lib) != 0) {
		printf("round %d: dlclose failed: %s\n", round, dlerror());
		return false;
	}
	void* still = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
	printf("round %d (%s): %s, %s on dlclose\n", round, appType == Scene ? "scene" : "background", ok ? "ok" : "failed",
	    still ? "stayed loaded" : "unloaded");
	if (still)
		dlclose(still);
	return ok;
}

} // namespace

int main(int argc, char** argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <vrclient.so>\n", argv[0]);
		return 2;
	}
	setvbuf(stdout, nullptr, _IONBF, 0);
	// A scene app twice, then a background app and a scene app again: each in a fresh load of the library
	const int types[] = { Scene, Scene, Background, Scene };
	bool ok = true;
	for (int i = 0; i < (int)(sizeof(types) / sizeof(types[0])); i++)
		ok = Round(argv[1], i + 1, types[i]) && ok;
	// Last, one more that the process exits in: exit's teardown is the test
	ok = Round(argv[1], 5, Scene, false) && ok;
	printf("%s\n", ok ? "all rounds worked" : "a round failed");
	return ok ? 0 : 1;
}
