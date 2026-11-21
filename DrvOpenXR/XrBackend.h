//
// Created by ZNix on 25/10/2020.
//

#pragma once

#include "CpuSwapchain.h"

#include <chrono>

#include "XrDriverPrivate.h"

#include "../OpenOVR/Misc/Fade.h"
#include "OwnFrameLoop.h"
#include "XrController.h"
#include "XrHMD.h"

#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <mutex>

class XrGenericTracker;
struct ID3D11Texture2D;

class XrBackend : public IBackend {
public:
	DECLARE_BACKEND_FUNCS(virtual, override)

	uint32_t CurrentFrameId() override;
	float SecondsToFrame(uint32_t frameId) override;
	void SetSceneOverlay(std::shared_ptr<const SceneOverlayImage> overlay) override;

	/** The display slot (count of display periods) a display time falls in; 0 where the period is unknown */
	static uint64_t DisplaySlot(XrTime displayTime, XrDuration period);
	/** The display slot of a frame after one at previous (shown at previousTime; 0 for none): the display periods
	 * between them on, at least one, so that a change of the display's rate doesn't move the count back or far on.
	 * The first is DisplaySlot's; 0 where the period is unknown. */
	static uint64_t NextDisplaySlot(uint64_t previous, XrTime previousTime, XrTime displayTime, XrDuration period);
	/** Seconds from now to the display of the frame `slot`, from the frame in flight's slot and display time; NaN where
	 * unknown. Slots are compared as 32-bit frame ids. */
	static double SecondsToSlot(uint32_t slot, uint32_t frameSlot, XrTime frameDisplayTime, XrDuration period, XrTime now);
	/** The display periods from now to a frame's display, at least one; 0 where unknown */
	static uint32_t VsyncsToDisplay(XrTime displayTime, XrDuration period, XrTime now);

	XrBackend(bool useVulkanTmpGfx, bool useD3D11TmpGfx);
	~XrBackend() override;

	/**
	 * The current state of the OpenXR session, or XR_SESSION_STATE_UNKNOWN if there is no session.
	 */
	XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;

	XrSessionState GetSessionState();

	/**
	 * Whether the session is active or not. This cannot be determined from just the session state, since
	 * we're allowed to send frames after calling xrBeginSession but before the event comes through. Same
	 * with ending the session - we're allowed to submit frames after we receive the stopping event, until
	 * we call xrEndSession.
	 */
	std::atomic<bool> sessionActive{ false };
	std::atomic<bool> sessionLost{ false };
	std::atomic<bool> exitExpected{ false };
	// The user quit from the runtime: no session is made again
	std::atomic<bool> quitting{ false };
	// The session went through STOPPING since it last ran (an EXITING after it is the user quitting)
	std::atomic<bool> sawStopping{ false };
	// The game has been told the devices are deactivated (a loss), and not yet that they're back
	std::atomic<bool> devicesDeactivated{ false };
	// The instance was lost with the session (XR_ERROR_INSTANCE_LOST, or INSTANCE_LOSS_PENDING): made again first
	std::atomic<bool> instanceLost{ false };
	// The instance loss: lost with the session (OnSessionLost)
	void OnInstanceLost();
	// The last try to find the system again (RecoverLostSession), and whether the lost session is destroyed yet
	std::atomic<std::chrono::steady_clock::rep> lastRecoveryTry{ 0 }; // 0 before the first
	bool lostSessionDestroyed = false;
	std::mutex recoveryLock;
	// What the game has been told of the session, as SteamVR tells it: whether it lost input focus (sessionEvents), and
	// the IPD (NaN until the session's first)
	std::atomic<bool> inputFocusLost{ false };
	// The session has been FOCUSED: only then is leaving that state the runtime taking input
	std::atomic<bool> inputFocusHad{ false };
	// The headset's standby, as the game was told of it (the session stopping), and the domains (bit n for
	// XrPerfSettingsDomainEXT n) the runtime reports above normal load
	std::atomic<bool> standbyTold{ false };
	std::atomic<uint32_t> perfStrainedDomains{ 0 };
	std::atomic<float> toldIpd{ NAN };
	// The app's graphics device couldn't be bound: the session stays on the temporary device
	std::atomic<bool> appGraphicsUnusable{ false };
	// It's on another adapter than the runtime's: if its session or swapchains fail, Submit answers SharedTexturesNotSupported
	std::atomic<bool> appOnOtherAdapter{ false };

	/**
	 * To be called after xrCreateSession. Should only be used by DrvOpenXR.
	 */
	void OnSessionCreated();

	// DrvOpenXR::KeepOverlaysInstance
	void KeepOverlaysInstance();

	/** Why the session is made again (RebuildSession) */
	enum class RebuildReason {
		AppGraphics, // the game's first Submit: from the temporary device to the game's own
		Inputs, // the game's actions, which can only be attached to a session that has none yet
		Loss, // the runtime lost the session (or one couldn't be made): a new one once the system is back
	};
	static const char* RebuildReasonName(RebuildReason reason);

	/**
	 * Makes the session again, under the session lock: never in the middle of a frame (a frame the game has open is
	 * ended first, as if it had submitted). What the game set on the old session is put back on the new one: its eye
	 * compositors are made again at its next Submit, the skybox is shown again from the game's texture, and the overlays'
	 * swapchains are made again from their textures. XR_SUCCESS, or what xrCreateSession answered.
	 */
	XrResult RebuildSession(RebuildReason reason);

	/**
	 * The session is lost (LOSS_PENDING, XR_ERROR_SESSION_LOST, an EXITING the user didn't ask for, or a session that
	 * couldn't be made): no more frames; the headset and controllers are deactivated, their poses invalid, until
	 * RecoverLostSession makes a new session. The game isn't told to quit.
	 */
	void OnSessionLost();
	/** Pumps events until done() is true, for at most timeoutMs: whether it was */
	bool WaitForEvents(const std::function<bool()>& done, int timeoutMs);
	/**
	 * The user quit from the runtime (EXITING after STOPPING): VREvent_DriverRequestedQuit and VREvent_Quit, once, and
	 * no session is made again
	 */
	void OnRuntimeQuit();
	/**
	 * Whether the game's actions (its manifest's, or legacy input's) and the info set may be attached now: once the
	 * session is on the game's device (or will stay where it is: a helper program's, or a game whose device can't be
	 * used). Before that the session restart for its first Submit would follow, and one more for a manifest it loads by
	 * then. A game that hasn't submitted within kInputsWithoutSubmit of first being asked about gets them all the same.
	 */
	static bool InputsMayAttach();
	static constexpr std::chrono::seconds kInputsWithoutSubmit{ 2 };

	/** Whether the current backend's session is gone (lost, or not made again yet): the devices have no poses */
	static bool CurrentSessionGone();
	/** Whether the current backend's session is lost and is to be made again (not the user quitting) */
	static bool CurrentSessionLost();
	/** Whether the user quit from the runtime (OnRuntimeQuit): the session isn't made again */
	bool IsQuitting() const { return quitting; }

	/** What an unasked session state change means for the game */
	enum class StateOutcome {
		Ignored,
		Lost,
		Quit,
	};
	/**
	 * For a state the session went to: LOSS_PENDING is a loss; EXITING is the user quitting if the session went
	 * through STOPPING since it last ran (sawStopping), a loss otherwise; neither while OpenLamina ends the session
	 * itself (exitExpected)
	 */
	static StateOutcome OutcomeOfState(XrSessionState state, bool sawStopping, bool exitExpected);

	/**
	 * Whether the event pump looks for the controllers itself, with the info set: on a session whose frames have
	 * started (SYNCHRONIZED on), for a game that has loaded no actions and asked for no legacy input, while neither
	 * hand has a controller. Not while OpenLamina ends the session (exitExpected), nor while it makes the session
	 * again for the game's actions (restartingForInputs).
	 */
	static bool ControllerQueryDue(XrSessionState state, bool exitExpected, bool restartingForInputs, bool actionsLoaded,
	    bool legacyInputAsked, bool handFound);
	// MaybeRestartForInputs is making the session again, on the thread that holds the session lock
	std::atomic<bool> restartingForInputs{ false };

	/** The wait between tries to find the system again after a loss */
	static constexpr std::chrono::milliseconds kRecoveryInterval{ 1000 };
	/** Whether a try is due at now: the first at once, then one each kRecoveryInterval (lastTry is {} before the first) */
	static bool RecoveryDue(std::chrono::steady_clock::time_point lastTry, std::chrono::steady_clock::time_point now);

	/**
	 * After a loss, on a thread that holds no session lock and no frame: destroys the lost session (once), then about
	 * once a second asks for the system (xrGetSystem) and, once it's there, makes the session again (RebuildSession);
	 * the devices are activated again. Retried for as long as it takes; nothing after the user quit. passLock is the
	 * helper loop's pass lock, taken after the session lock.
	 */
	void RecoverLostSession(std::mutex* passLock = nullptr);
	bool IsSessionLost() const { return sessionLost; }
	/** The session is lost, not running, or stopping, exiting or about to be lost: nothing can be shown in it any more */
	bool IsSessionEnded() const
	{
		return sessionLost || !sessionActive || sessionState == XR_SESSION_STATE_STOPPING || sessionState == XR_SESSION_STATE_EXITING
		    || sessionState == XR_SESSION_STATE_LOSS_PENDING;
	}

	/** Called before OpenComposite asks the runtime to end the session itself, so its EXITING isn't a loss */
	void ExpectSessionExit() { exitExpected = true; }

	/** False if neither D3D11 nor Vulkan could start the session (CreateOpenXRBackend then gives up) */
	bool HasTemporaryGraphics() const { return temporaryGraphics != nullptr; }

	void PrepareForSessionShutdown();

	const void* GetCurrentGraphicsBinding();

	/**
	 * An overlay application's session: made at Init, for good, on OpenLamina's own device (D3D11 on the runtime's
	 * adapter where the runtime has D3D11, else Vulkan), which no thread of the program uses. False if there's no
	 * session to be had (logged).
	 */
	bool EnsureOverlaySession();

	/**
	 * An overlay application's texture copied onto the overlay session's device, on the program's thread
	 * (OverlayTextureCopy): nullptr, with why, if it can't be
	 */
	ID3D11Texture2D* CopyOverlayTexture(const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds, std::string* why, const void* owner);
	// The overlay (CopyOverlayTexture's owner) shows no texture any more: what was kept to copy its textures is let go
	void ForgetOverlayTextures(const void* owner);
	/**
	 * For an overlay application that reads its overlay's texture back and writes it directly: a shared texture of
	 * like's device with like's contents (program) and the same texture on the overlay session's device (session),
	 * both referenced for the caller (OverlayTextureCopy::MakeLive). False, with why, if it can't be made.
	 */
	bool MakeLiveOverlayTexture(ID3D11Texture2D* like, ID3D11Texture2D** program, ID3D11Texture2D** session, std::string* why);
	/** A DXGI shared handle's texture on the overlay session's device, referenced for the caller; nullptr, with why */
	ID3D11Texture2D* OpenSharedOverlayTexture(void* handle, std::string* why);
	/** A compositor for such a copy */
	std::shared_ptr<Compositor> MakeOverlayCompositor(ID3D11Texture2D* copy);

	/**
	 * Where the overlay session's device is Vulkan (Linux): an overlay application's Vulkan or OpenGL texture copied
	 * into an image of that device that shares its memory with the program's device or context (OverlayTextureShare),
	 * as a Vulkan texture of the session's device; flipY if its rows run bottom-up. nullptr, with why, if it can't be:
	 * the texture is read back instead. ReleaseSharedOverlayTexture once the session's copy from it is submitted.
	 */
	const vr::VRVulkanTextureData_t* ShareOverlayTexture(const vr::Texture_t* texture, const void* owner, bool* flipY, std::string* why);
	void ReleaseSharedOverlayTexture(const void* owner);
	/** A compositor for a texture of the session's own device (ShareOverlayTexture's) */
	std::shared_ptr<Compositor> MakeSessionOverlayCompositor(const vr::Texture_t* texture);

	/**
	 * Whether the thread running OpenLamina's frames fills the swapchains drawn from memory (and the pointer's): in an
	 * overlay application, whose session is on OpenLamina's own device. In a game, the game's thread does, as it uses the
	 * session's device itself.
	 */
	bool OwnFramesFillSwapchains() const;

	void RegisterOverlayCompositor(std::shared_ptr<Compositor> compositor);
	void UnregisterOverlayCompositor(std::shared_ptr<Compositor> compositor);

	/**
	 * Restarts the session to allow for inputs to be attached to the session, if necessary.
	 * To be called from BaseInput whenever it's attempting to attach the game actions.
	 * Restarting the session should only be necessary if we've already created the infoSet.
	 */
	static void MaybeRestartForInputs();

	/**
	 * One frame of OpenLamina's own (OwnFrameLoop), with source's layers: on the calling thread, which mustn't hold
	 * the frame (frameOwner) already. Runs none, answering 0, if wanted() turns false first, if the session isn't
	 * running, or if the game's frame is open (begun and not yet ended: the game ends it). Otherwise answers the frame's
	 * predicted display period.
	 */
	XrDuration RunOwnFrame(const OwnFrameLoop::LayerSource& source, const std::function<bool()>& wanted);

	/** The runtime's last predicted display period (from xrWaitFrame), 0 before the first */
	float DisplayPeriodMs() const { return displayPeriodMs; }

	/**
	 * Stops OpenLamina's own frames (and joins their thread) until the next session is created, or for good when
	 * OpenLamina shuts down (before the interfaces their layers come from are destroyed)
	 */
	void StopOwnFrames(bool forGood = false);

	/** Whether own frames can run on the calling thread now: there are some, and on a session whose frames run only
	 * inside the game's calls, this is its render thread (OpenGL: the session's context is current here; Vulkan: the
	 * thread that last waited for a frame, so none before the game's first) */
	bool OwnFramesHere() const;

	/**
	 * Runs OpenLamina's own frames on the calling thread while keepGoing() holds (OwnFrameLoop::RunWhile): a blocking
	 * call such as ShowMessageOverlay. The caller holds the session lock (shared) throughout, so the loop isn't stopped
	 * under it. False, running nothing, without own frames.
	 */
	bool RunOwnFramesWhile(const std::function<bool()>& keepGoing);

	/**
	 * Ends the game's frame if it's in the middle of one (begun, not yet submitted), with the overlays and without
	 * its eye textures, so that own frames can run: for a blocking call made between WaitGetPoses and Submit
	 */
	void EndOpenAppFrame();

#ifdef SUPPORT_VK
	static void VkGetPhysicalDevice(VkInstance instance, VkPhysicalDevice* out);
#endif

	/** The tracker action is attached: the trackers by role again */
	void OnTrackerActionAttached() { CreateGenericTrackers(); }
	/** The session ended, or a new one is being set up: the trackers' spaces went with the old one, and are made again
	 * in the new. The caller holds the session lock exclusively. */
	void ForgetTrackerSpaces();

private:
	std::shared_ptr<XrHMD> hmd = std::make_shared<XrHMD>();
	// The stage's bounds rectangle, asked of the runtime once per session and again after the stage changes
	// (XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING); under playAreaLock
	std::mutex playAreaLock;
	XrSession playAreaSession = XR_NULL_HANDLE;
	bool playAreaKnown = false;
	XrExtent2Df playAreaRect{};
	// Until the stage's change takes effect (its event's changeTime), the bounds are asked each time, not kept
	XrTime playAreaChangeTime = 0;
	void ForgetPlayArea(XrTime changeTime = 0);
	// Replaced when a hand's interaction profile changes, while other threads read input: copied and replaced only
	// under hands_mutex (Hand), which is held for nothing else
	std::shared_ptr<XrController> hand_left;
	std::shared_ptr<XrController> hand_right;
	mutable std::shared_mutex hands_mutex;
	std::shared_ptr<XrController> Hand(XrController::XrControllerType hand) const
	{
		std::shared_lock lock(hands_mutex);
		return hand == XrController::XCT_LEFT ? hand_left : hand_right;
	}

	std::vector<std::shared_ptr<XrGenericTracker>> generic_trackers;
	std::shared_mutex generic_trackers_mutex;
	// The session the trackers' spaces belong to, and the xdev list generation they were made from
	XrSession generic_trackers_session = XR_NULL_HANDLE;
	uint64_t generic_trackers_action_generation = 0; // of the tracker action the role trackers' spaces were made from
	std::atomic<uint64_t> generic_trackers_generation{ UINT64_MAX };
	std::atomic<int64_t> generic_trackers_polled_ms{ 0 };
	// Makes the trackers again when the runtime's list of devices has changed (looked at about once a second)
	void PollGenericTrackers();

	// False if the texture can't be used (logged): no session switch then, or the switch was undone
	// moveToNewDevice: the session is made again on the texture's device (FollowEyeTextureDevice), once the checks
	// that can be made before the old session goes down have passed
	bool CheckOrInitCompositors(const vr::Texture_t* tex, bool moveToNewDevice = false);
	// The texture is of the session's API (D3D11 or D3D12) but on another device than the session's
	bool TextureOnOtherDevice(const vr::Texture_t* tex);
	// Whether an eye texture's device is followed if the game moved to another one (another GPU, or a device made again
	// after a device loss): move set if it is to be; false if it's on another device and can't be, and the eye is left out
	bool FollowEyeTextureDevice(const vr::Texture_t* tex, bool& move);
	// The session is being made again on the device a game moved to: if that fails, the game isn't told to quit
	std::atomic<bool> rebuildingForNewDevice{ false };
	std::atomic<int> deviceRebuilds{ 0 };
	// Makes the temporary graphics device the backend was made with, if there's none: false if it can't be made
	static bool MakeTemporaryGraphics();
	std::unique_ptr<Compositor> compositors[XruEyeCount];
	// The D3D11 mirror texture of each eye (an ID3D11Texture2D of OpenLamina's own on the game's device), which the
	// game's frames are copied into once it has asked for one (mirrorWanted); under mirrorLock
	void* mirrorTexture[XruEyeCount]{};
	bool mirrorWanted = false;
	// The format of each eye's frames (a DXGI_FORMAT), for a mirror texture made after them
	int mirrorFormat[XruEyeCount]{};
	std::mutex mirrorLock;
	std::set<void*> mirrorViews; // handed out by GetMirrorTextureD3D11 and not yet released
	// A frame's texture of the eye, submitted with these bounds; with none, the eye's mirror texture is let go
	void KeepForMirror(vr::EVREye eye, const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds);
	std::unique_ptr<Compositor> skybox_compositor; // the quad shown without equirect layers

	// The skybox SetSkyboxOverride set, shown in OpenLamina's own frames until ClearSkyboxOverride: one or two equirect
	// layers. Clearing only stops showing it; its swapchains are refilled under frameOwner (which own frames hold) by the
	// next skybox, or let go of with the session. Under skyboxLock.
	struct Skybox {
		std::unique_ptr<Compositor> textures[2]; // lat-long textures, one per eye for a stereo pair
		std::unique_ptr<CpuSwapchain> faces; // six faces, as one lat-long image joined on the CPU
		std::unique_ptr<Compositor> cube; // six faces, as one lat-long image drawn on the GPU (InvokeCubemap)
		XrCompositionLayerEquirect2KHR layers[2]{};
		int layerCount = 0;
	};
	std::mutex skyboxLock;
	Skybox skybox;
	std::unique_ptr<Compositor> skyboxReader; // reads cube faces back
	// The game's last projection layer, for OpenLamina's own frames; under frameOwner
	XrCompositionLayerProjection lastProjection{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	XrCompositionLayerProjectionView lastProjectionViews[2]{};
	std::atomic<bool> haveLastProjection{ false };
	// SubmitFrames shows the game's last frame again if this frame has none of its own (a frame the game didn't submit
	// to, ended for it)
	std::atomic<bool> repeatLastFrame{ false };
	// An OpenGL session's context, which its own frames (run only inside the game's calls) need current; else null
	std::atomic<const void*> ownFramesGlContext{ nullptr };
	// The game's OpenGL context on Linux, current when its session's binding was made
	const void* gameGlContext = nullptr;
	// A Vulkan session's own frames run inside the game's calls on the thread it renders on (the last to wait for a
	// frame), where OpenVR may use its queue
	std::atomic<bool> ownFramesInCalls{ false };
	std::atomic<std::thread::id> gameFrameThread{};
	bool SkyboxShown();
	// The layers of the frame being ended, under frameOwner
	std::vector<const XrCompositionLayerBaseHeader*> frameLayers;
	// The scene fade as a layer: a quad over the whole view, its texels the fade's colour and alpha. Under frameOwner.
	std::unique_ptr<CpuSwapchain> fadeSwapchain;
	uint8_t fadeTexel[4]{};
	XrCompositionLayerQuad fadeLayer{ XR_TYPE_COMPOSITION_LAYER_QUAD };
	// The fade layer for the fade as it is now, or nullptr while nothing is faded (or it can't be drawn)
	XrCompositionLayerBaseHeader* BuildFadeLayer();
	// The old compositor calls' scene overlay (SetSceneOverlay): set from any thread under sceneOverlayLock, drawn
	// under frameOwner from a swapchain filled from its image
	std::mutex sceneOverlayLock;
	std::shared_ptr<const SceneOverlayImage> sceneOverlay;
	std::shared_ptr<const SceneOverlayImage> sceneOverlayShown; // the one in the swapchain
	std::unique_ptr<CpuSwapchain> sceneOverlaySwapchain;
	XrCompositionLayerQuad sceneOverlayLayer{ XR_TYPE_COMPOSITION_LAYER_QUAD };
	// The scene overlay's layer, or nullptr while there's none (or it can't be drawn)
	XrCompositionLayerBaseHeader* BuildSceneOverlayLayer();
	// A skybox asked for while the session wasn't running, shown at the next frame's start; under pendingSkyboxLock
	std::mutex pendingSkyboxLock;
	SkyboxRequest pendingSkybox;
	// The skybox shown until ClearSkyboxOverride, shown again on a new session (RebuildSession); under pendingSkyboxLock
	SkyboxRequest shownSkybox;
	bool EquirectSkyboxAvailable() const;
	// The session lock held (shared) and the session running
	IBackend::openvr_enum_t ShowSkybox(const vr::Texture_t* textures, uint32_t count);
	IBackend::openvr_enum_t ShowSkyboxQuad(const vr::Texture_t* texture);
	void ShowPendingSkybox();
	// The skybox from lat-long textures or cube faces; an OpenVR compositor error
	int SetEquirectSkybox(const vr::Texture_t* textures, uint32_t count);
	std::vector<std::shared_ptr<Compositor>> overlay_compositors;

	/**
	 * Updates the current interaction profile in use according to the runtime.
	 * This will set the XrHMD's interaction profile, as well as create the XrControllers
	 * and set their interaction profiles accordingly.
	 * This allows for games to retrieve correct per-controller OpenVR properties that they request.
	 * Called from PumpEvents on an INTERACTION_PROFILE_CHANGED event.
	 */
	void UpdateInteractionProfile();

	/**
	 * The Vive trackers by role (XR_HTCX_vive_tracker_interaction, their poses from the tracker action) and the xdevs
	 * (XR_MNDX_xdev_space) the runtime doesn't also list with a role. Ignores HMD & Controllers.
	 * These "Generic Trackers" act like an HTC vive tracker, but support no bindings.
	 */
	void CreateGenericTrackers();
	/** The hand's profile as a controller (XR_EXT_hand_interaction's) if hand tracking tracks it (0 left, 1 right),
	 * null if not */
	const InteractionProfile* HandWithoutController(int hand);
	/** Once a second: whether a hand without a controller came or went */
	bool HandsWithoutControllersChanged();
	std::atomic<int64_t> handsCheckedMs{ 0 };
	// The checks in a row each hand without a controller wasn't tracked in, and how many it may miss
	std::atomic<int> handMisses[2] = { 0, 0 };
	// Whether each hand's controller is the hand itself, shown because hand tracking tracks it
	std::atomic<bool> handFromTracking[2] = { false, false };
	// The Vive trackers the runtime listed, to notice one going (no event says so)
	std::atomic<uint32_t> viveTrackersListed{ 0 };
	static constexpr int kHandMissesAllowed = 3;
	/** The xdevs that are trackers, in a list it makes (null if it can't), each passed to add; those whose lower-case
	 * serial ends one of byRole (the lower-case persistent paths of the Vive trackers listed by role) are left out;
	 * false if they can't be listed */
	bool FindXDevTrackers(XrXDevListMNDX& list, const std::vector<std::string>& byRole,
	    const std::function<void(XrXDevIdMNDX, const XrXDevPropertiesMNDX&)>& add);

	/**
	 * Attempts to force the runtime to expose an interaction profile
	 * (i.e., send an INTERACTION_PROFILE_CHANGED event).
	 * If this is called before the game's actions have been attached to the session by BaseInput
	 * (which is the case for essentially all legacy input games), the session will have to be restarted
	 * to attach the actual inputs. BaseInput handles this when attaching its inputs.
	 */
	void QueryForInteractionProfile();
	void CreateInfoSet();
	void BindInfoSet();

	// Whether we've restarted the session to use the application's rendering API yet
	std::atomic<bool> usingApplicationGraphicsAPI{ false };
	// When InputsMayAttach was first asked before a Submit (steady_clock ticks; 0 before)
	std::atomic<std::chrono::steady_clock::rep> inputsFirstAsked{ 0 };
	// An overlay application's textures, copied onto its session's device
	std::unique_ptr<class OverlayTextureCopy> overlayCopy;
#if defined(SUPPORT_VK) && !defined(_WIN32)
	std::unique_ptr<class OverlayTextureShare> overlayShare;
#endif

	// A helper program's frame loop (overlay sessions) or event loop (headless ones), made with each session
	void StartHelperLoop();
	void StartBackgroundFrames();
	// Held through each of its passes
	std::mutex helperPassLock;

	// The overlays' instance, made at Init for a game whose lasers point at other programs' overlays (laserMouse) or
	// which has a dashboard (dashboardButton) or the play area's walls (drawBounds), until OpenLamina shuts down
	std::shared_ptr<class BaseOverlay> keptOverlay;

	// The settings' generation the render scale (supersampleScale, the app's resolutionScale) was last read at, and the
	// scale the game was last told of
	bool renderScaleRead = false;
	uint64_t settingsGeneration = 0;
	float notifiedRenderScale = 0;
	// Between the game's frames: a render scale another program set reaches the game, as SteamVR tells it, with
	// VREvent_SteamVRSectionSettingChanged; its next textures at the new recommended size get swapchains of that size
	void NoticeRenderScale();
	// Between the game's frames, at the session's start and when the settings change: the display rate nearest the
	// user's preferred one (steamvr.preferredRefreshRate) asked of the runtime, where it takes requests. Without a
	// preference the runtime's own choice stays.
	void ApplyPreferredRefreshRate();
	bool refreshRateChecked = false;
	uint64_t refreshRateGeneration = 0;
	float requestedRefreshRate = 0;

	// The views for the two main eye layers
	XrCompositionLayerProjectionView projectionViews[XruEyeCount];

	// The pose a frame was rendered with (useSubmittedPose): the submitted head pose times the eye's pose in the head
	void UseSubmittedPose(vr::EVREye eye, const vr::Texture_t* texture, vr::EVRSubmitFlags submitFlags);
	// The depth that goes with a view (submitDepth), the bounds' area of it as of the colour, chained to it for this frame
	void SubmitDepth(vr::EVREye eye, const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds, vr::EVRSubmitFlags submitFlags);
	XrCompositionLayerDepthInfoKHR depthInfos[XruEyeCount] = {};
	std::unique_ptr<Compositor> depthCompositors[XruEyeCount];

	// Have we started rendering a frame yet? If not, calling xrEndFrame would result in an error
	bool renderingFrame = false;

	// Who runs the frame: held from xrWaitFrame to xrEndFrame by OpenLamina's own frames, and around each frame call
	// of the game's (its xrWaitFrame and xrBeginFrame, its xrEndFrame), whose frame is open in between (frameBegun), so
	// that no frame of one starts while the other's is open. Taken after the session lock (shared), and never held while
	// waiting for the session lock or for the own frames' thread.
	std::mutex frameOwner;
	// A frame was begun (by the game, or SetSkyboxOverride) and not yet ended; under frameOwner
	std::atomic<bool> frameBegun{ false };
	// OpenLamina's frames while the game runs none; created with each session, stopped before it ends
	std::unique_ptr<OwnFrameLoop> ownFrames;
	bool ownFramesStopped = false; // for good: OpenLamina is shutting down

	// A frame xrWaitFrame gave whose xrBeginFrame failed: the next frame begins it again instead of waiting, as a
	// second xrWaitFrame blocks until xrBeginFrame is called. Repeated failures lose the session.
	bool frameAwaitingBegin = false;
	// Consecutive frames whose xrWaitFrame or xrBeginFrame failed
	std::atomic<int> frameCallFailures{ 0 };
	// Drops the frame, waits about a frame and, if the session is lost or five frames failed in a row, loses the session
	void OnFrameCallFailed(XrResult res);

	// Were we supposed to start rendering a frame, but couldn't since we were on the
	// early (pre switch to application graphics instance) OpenXR session?
	bool deferredRenderingStart = false;

	// Keep track of if eye textures have been submitted and if we need to create a projection layer for them
	bool submittedEyeTextures = false;

	// Whether the runtime shows the frame in flight (xrWaitFrame's shouldRender); under frameOwner
	bool frameShouldRender = true;
	// The space the frame's views were located in, which its projection layer is labelled with
	XrReferenceSpaceType frameSpace = XR_REFERENCE_SPACE_TYPE_STAGE;
	// The eyes submitted in the frame in flight
	bool eyeSubmitted[XruEyeCount] = {};
	// The game cleared its last frame (ClearLastSubmittedFrame), or suspended rendering: its eye textures aren't shown
	std::atomic<bool> sceneCleared{ false };
	std::atomic<bool> renderingSuspended{ false };

	// The game's frame in flight: its index (display slot where the runtime gives a display period, else a count of
	// frames), the slot's display time and the display period
	uint32_t nFrameIndex = 0;
	std::atomic<uint32_t> frameSlot{ 0 };
	uint64_t displaySlot = 0; // the last frame's, counted on by NextDisplaySlot
	XrTime displaySlotTime = 0;
	std::atomic<XrTime> frameDisplayTime{ 0 };
	std::atomic<XrDuration> frameDisplayPeriod{ 0 };
	// The display periods from xrWaitFrame's return to the frame's display
	uint32_t vsyncsToFirstView = 0;
	// The frame's xrWaitFrame call, the time it has spent in Submit and when its last eye came (from its start)
	std::chrono::steady_clock::time_point frameStart{}, frameWaitReturned{};
	float frameSubmitMs = 0.0f, frameNewFrameReadyMs = 0.0f;
	// The last ended frame's, for GetFrameTiming
	float lastSubmitMs = 0.0f, lastPresentMs = 0.0f, lastNewFrameReadyMs = 0.0f;

	double frameSubmitTimeUs = 0.0;

	// Measured frame timing for GetFrameTiming, taken around xrWaitFrame in WaitForTrackingData. Games such as
	// Half-Life: Alyx drive their dynamic resolution from these values, so constants freeze it at one level.
	// busy = time from one xrWaitFrame return to the next xrWaitFrame call (the app's render and submit), smoothed;
	// interval = time between xrWaitFrame calls, per frame.
	std::chrono::steady_clock::time_point lastWaitCall{};
	float measuredIntervalMs = 0.0f, measuredBusyMs = 0.0f, measuredWaitMs = 0.0f;
	std::atomic<float> displayPeriodMs{ 0.0f }; // also written by OpenLamina's own frames' thread
	uint32_t measuredDropped = 0;
	XrTime lastPredictedDisplayTime = 0;
	void RecordFrameTiming(std::chrono::steady_clock::time_point callTime, std::chrono::steady_clock::time_point returnTime,
	    XrTime predictedDisplayTime, XrDuration predictedDisplayPeriod);
	// Starts measuring again from the next frame
	void RestartFrameTiming()
	{
		lastWaitCall = {};
		lastPredictedDisplayTime = 0;
		measuredDropped = 0;
	}

	// Action set and action used for querying for the interaction profile
	inline static XrActionSet infoSet = XR_NULL_HANDLE;
	XrAction infoAction = XR_NULL_HANDLE;
	XrPath subactionPaths[2] = { XR_NULL_PATH, XR_NULL_PATH };

	// Abstract class for holding a graphics binding.
	class BindingBase {
	public:
		virtual const void* asVoid() = 0;
		virtual ~BindingBase() = default;
	};

	// A wrapper around a graphics binding type, subclassing BindingBase
	// It is templated to allow storing any of the possible graphics bindings and getting a void* to it,
	// as necessary for xrCreateSession (in DrvOpenXR::SetupSession)
	template <typename T>
	class BindingWrapper : public BindingBase {
		const T data;

	public:
		BindingWrapper(T data)
		    : data(data) {}
		~BindingWrapper() override
		{
#if defined(SUPPORT_GL) && !defined(_WIN32)
			if constexpr (std::is_same_v<T, struct XrGraphicsBindingOpenGLXlibKHR>)
				glXDestroyContext(data.xDisplay, data.glxContext);
#endif
		}
		const void* asVoid() override { return &data; }
	};

	// The current graphics binding, used for restarting the session
	inline static std::unique_ptr<BindingBase> graphicsBinding = nullptr;
	// OpenGL or Vulkan frames copied through D3D11, whose device graphicsBinding has (GfxInterop.h)
	std::unique_ptr<class GfxInterop> gfxInterop;
	static std::unique_ptr<class TemporaryGraphics> temporaryGraphics;
	// Which temporary graphics APIs the backend may use
	inline static bool tmpGfxVulkan = false, tmpGfxD3D11 = false;
};
