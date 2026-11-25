#pragma once

#include <mutex>
#include <string>

#include "ParallelProjections.h"

// openlamina.ini's options. The file is read at the first question, not when the library is loaded (the program's
// logging and working directory are its own by then).
class Config {
public:
	Config();
	~Config();

	bool RenderCustomHands() const { return Loaded().renderCustomHands; }
	vr::HmdColor_t HandColour() const { return Loaded().handColour; }
	float SupersampleRatio() const { return Loaded().supersampleRatio; }
	bool Haptics() const { return Loaded().haptics; }
	bool EyeTracking() const { return Loaded().eyeTracking; }
	bool AdmitUnknownProps() const { return Loaded().admitUnknownProps; }
	inline bool LogGetTrackedProperty() const { return Loaded().logGetTrackedProperty; }
	inline bool StopOnSoftAbort() const { return Loaded().stopOnSoftAbort; }
	inline bool EnableHiddenMeshFix() const { return Loaded().enableHiddenMeshFix; }
	inline bool InvertUsingShaders() const { return Loaded().invertUsingShaders; }
	inline bool InitUsingVulkan() const { return Loaded().initUsingVulkan; }
	/** With Submit_TextureWithPose: each eye's view is the submitted head pose times that eye's pose in the head */
	inline bool UseSubmittedPose() const { return Loaded().useSubmittedPose; }
	/** With Submit_TextureWithDepth, where the runtime has XR_KHR_composition_layer_depth: each view gets the depth */
	inline bool SubmitDepth() const { return Loaded().submitDepth; }
	/**
	 * On a headset whose eyes are turned outwards: the game is given eyes that look straight ahead, each with a field
	 * of view widened to take in the turned one, for games that render as if the eyes were never turned. The game
	 * list's choice for this program, else openlamina.ini's (auto, true or false), else on for the games known to need
	 * it (ParallelProjections.h).
	 */
	inline bool ParallelProjections() const { return Loaded().parallelProjectionsOn; }
	/** Frames of OpenLamina's own while the game runs none: the game's last frame again, its overlays and its skybox */
	inline bool OwnFrames() const { return Loaded().ownFrames; }
	/** A desktop window with the game's left eye, when the game asks for one (IVRCompositor::ShowMirrorWindow) */
	inline bool MirrorWindow() const { return Loaded().mirrorWindow; }
	/** The keyboard (IVROverlay::ShowKeyboard), pointed at with the controllers; without it, done at once */
	inline bool Keyboard() const { return Loaded().keyboard; }
	/** Overlays that take mouse input (VROverlayInputMethod_Mouse) are pointed at with the controllers as a mouse */
	inline bool LaserMouse() const { return Loaded().laserMouse; }
	/** SteamVR's render models from the user's install; without them, OpenLamina's built-in ones */
	inline bool SteamVRRenderModels() const { return Loaded().steamvrRenderModels; }
	/** Overlay and background applications get overlay and headless sessions, where the runtime has them */
	inline bool HelperApps() const { return Loaded().helperApps; }
	/** An overlay application's layers' place among other programs' (XrSessionCreateInfoOverlayEXTX) */
	inline uint32_t OverlayLayersPlacement() const { return Loaded().overlayLayersPlacement; }
	/**
	 * What opens OpenLamina's dashboard: "system" (either controller's system button, pressed), "system_long" (held
	 * 0.6 s), "menu" (the left menu button, pressed), "menu_long" (held 0.6 s) or "off"
	 */
	const std::string& DashboardButton() const { return Loaded().dashboardButton; }
	/** A grid wall on the play area's edge when the HMD or a controller comes near it */
	inline bool DrawBounds() const { return Loaded().drawBounds; }
	/** The events SteamVR sends when the game loses or regains input focus, and when the headset is put on or taken off */
	inline bool SessionEvents() const { return Loaded().sessionEvents; }
	/** On Monado, through libmonado: the room setup's standing zero pose moves the runtime's stage */
	inline bool ApplyZeroPoses() const { return Loaded().applyZeroPoses; }
	float HiddenMeshVerticalScale() const { return Loaded().hiddenMeshVerticalScale; }
	inline bool LogAllOpenVRCalls() const { return Loaded().logAllOpenVRCalls; }
	/** The program OpenBindingUI starts; empty for none */
	const std::string& BindingEditor() const { return Loaded().bindingEditor; }
	/** The keyboard's layout: "us", "uk", or empty to choose by the user's locale (UK for en-GB, else US) */
	const std::string& KeyboardLayout() const { return Loaded().keyboardLayout; }
	/** The controller the hands are shown as when no controller is in them: "knuckles" (an Index controller),
	 * "oculus_touch", "vive_controller", or "off" for none */
	const std::string& HandController() const { return Loaded().handController; }

private:
	// Reads the files, once, and answers this
	const Config& Loaded() const;
	void Read();
	mutable std::once_flag loaded;

	static int ini_handler(
	    void* user, const char* section,
	    const char* name, const char* value,
	    int lineno);

	bool renderCustomHands = true;
	vr::HmdColor_t handColour = vr::HmdColor_t{ 0.3f, 0.3f, 0.3f, 1 };
	float supersampleRatio = 1.0f;
	bool haptics = true;
	bool eyeTracking = true;
	bool admitUnknownProps = false;
	bool logGetTrackedProperty = false;
	bool stopOnSoftAbort = false;
	bool enableHiddenMeshFix = true;
	bool invertUsingShaders = false;
	bool initUsingVulkan = false;
	bool useSubmittedPose = true;
	bool submitDepth = true;
	parallel_projections::Choice parallelProjections = parallel_projections::Choice::Automatic;
	bool parallelProjectionsOn = false;
	bool ownFrames = true;
	bool mirrorWindow = false;
	bool keyboard = true;
	bool laserMouse = true;
	bool steamvrRenderModels = true;
	bool helperApps = true;
	uint32_t overlayLayersPlacement = 1;
	std::string dashboardButton = "system";
	bool drawBounds = true;
	bool sessionEvents = true;
	bool applyZeroPoses = true;
	float hiddenMeshVerticalScale = 1.0f;
	bool logAllOpenVRCalls = false;
	std::string bindingEditor;
	std::string keyboardLayout;
	std::string handController = "knuckles";
};

extern Config oovr_global_configuration;
