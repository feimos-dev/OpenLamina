#pragma once

#include <string>

class Config {
public:
	Config();
	~Config();

	bool RenderCustomHands() const { return renderCustomHands; }
	vr::HmdColor_t HandColour() const { return handColour; }
	float SupersampleRatio() const { return supersampleRatio; }
	bool Haptics() const { return haptics; }
	bool EyeTracking() const { return eyeTracking; }
	bool AdmitUnknownProps() const { return admitUnknownProps; }
	inline bool LogGetTrackedProperty() const { return logGetTrackedProperty; }
	inline bool StopOnSoftAbort() const { return stopOnSoftAbort; }
	inline bool EnableHiddenMeshFix() const { return enableHiddenMeshFix; }
	inline bool InvertUsingShaders() const { return invertUsingShaders; }
	inline bool InitUsingVulkan() const { return initUsingVulkan; }
	/** With Submit_TextureWithPose: each eye's view is the submitted head pose times that eye's pose in the head */
	inline bool UseSubmittedPose() const { return useSubmittedPose; }
	/** With Submit_TextureWithDepth, where the runtime has XR_KHR_composition_layer_depth: each view gets the depth */
	inline bool SubmitDepth() const { return submitDepth; }
	/** Frames of OpenLamina's own while the game runs none: the game's last frame again, its overlays and its skybox */
	inline bool OwnFrames() const { return ownFrames; }
	/** The keyboard (IVROverlay::ShowKeyboard), pointed at with the controllers; without it, done at once */
	inline bool Keyboard() const { return keyboard; }
	/** Overlays that take mouse input (VROverlayInputMethod_Mouse) are pointed at with the controllers as a mouse */
	inline bool LaserMouse() const { return laserMouse; }
	/** SteamVR's render models from the user's install; without them, OpenLamina's built-in ones */
	inline bool SteamVRRenderModels() const { return steamvrRenderModels; }
	float HiddenMeshVerticalScale() const { return hiddenMeshVerticalScale; }
	inline bool LogAllOpenVRCalls() const { return logAllOpenVRCalls; }
	/** The program OpenBindingUI starts; empty for none */
	const std::string& BindingEditor() const { return bindingEditor; }
	/** How IVRInput evaluates a game's binding files: "direct" (default, each binding turned into OpenXR
	 * bindings) or "steamvr" (OpenLamina evaluates SteamVR's modes on the controller's raw inputs) */
	std::string InputEvaluator() const { return inputEvaluator; }
	/** The keyboard's layout: "us", "uk", or empty to choose by the user's locale (UK for en-GB, else US) */
	const std::string& KeyboardLayout() const { return keyboardLayout; }

private:
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
	bool dx10Mode = false; // read but not used: Direct3D 10 textures are told apart from Direct3D 11 ones by their interface
	bool enableHiddenMeshFix = true;
	bool invertUsingShaders = false;
	bool initUsingVulkan = false;
	bool useSubmittedPose = false;
	bool submitDepth = false;
	bool ownFrames = false;
	bool keyboard = false;
	bool laserMouse = false;
	bool steamvrRenderModels = false;
	float hiddenMeshVerticalScale = 1.0f;
	bool logAllOpenVRCalls = false;
	std::string bindingEditor;
	std::string inputEvaluator = "direct";
	std::string keyboardLayout;
};

extern Config oovr_global_configuration;
