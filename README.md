<p align="center">
  <img src="art/icon/icon.png" width="128" height="128" alt="OpenLamina's icon: a pane of glass, notched at one corner, over a blue sheet">
</p>

<h1 align="center">OpenLamina</h1>

<p align="center">
  <a href="https://github.com/feimos-dev/OpenLamina/actions/workflows/build.yml"><img src="https://img.shields.io/github/actions/workflow/status/feimos-dev/OpenLamina/build.yml?branch=main&style=for-the-badge&logo=github&label=build" alt="Build"></a>
  <a href="https://github.com/feimos-dev/OpenLamina/releases"><img src="https://img.shields.io/github/v/release/feimos-dev/OpenLamina?include_prereleases&style=for-the-badge&label=release" alt="Release"></a>
</p>

Play SteamVR (OpenVR) games on any OpenXR runtime, without SteamVR.

OpenLamina is a fork of [OpenComposite](https://gitlab.com/znixian/OpenOVR) by ZNix and its contributors. It has a name
of its own because it has grown well past it, and its code isn't kept in step with upstream's any more. Upstream hasn't
merged anything since July 2025, and its open merge requests, some of them needed by newer games such as Half-Life:
Alyx, have waited without review. OpenLamina takes them, fixes what review found in them, adds fixes of its own, and
publishes Windows and Linux builds.

**Bugs and questions about this fork go to [its issues](https://github.com/feimos-dev/OpenLamina/issues)**, not to
upstream's GitLab or Discord as they can't help with changes they haven't made.

## What this fork has that upstream doesn't

Upstream merge requests are linked as !*n* with their authors. Everything else was written for this fork.

**Games keep running**
- Nothing OpenLamina does ends a game any more. A call it can't answer properly gets SteamVR's error or an empty
  answer, is logged once, and is listed in a compatibility summary at the end of the log. Failed OpenXR calls, a lost
  session and swapchains that can't be made are recovered from. A broken `openlamina.ini`, action manifest or
  binding file costs only the entries that are wrong. CI checks that no new hard stop gets in.
- Every OpenVR interface version Valve has shipped, from SDK 0.9.0 on, is offered (or set aside with a reason in
  `tests/conformance/not_implemented.txt`). Old versions get the old struct layouts.
- Settings, the application registry, room setup and their events are kept for real and shared by every program using
  OpenLamina, as SteamVR shares them.
- SteamVR's binding files are evaluated by OpenLamina itself, with SteamVR's binding modes (toggles, dpads, chords and
  the rest), on the controllers' raw inputs.
- A conformance test program runs every method of every interface version and compares the answers with SteamVR's.

**Newer games**
- The interfaces of OpenVR SDK 2.12 and 2.15: `IVRSystem_023` and `_026`, `IVRCompositor_029`, `IVRInput_011`,
  `IVROverlay_028`, `IVRApplications_008` and `IVRNotifications_002`. Games built with those SDKs (Half-Life: Alyx,
  No Man's Sky, Batman: Arkham VR, Portal 2: Community Edition and others) no longer stop at "Missing interface".
  ([!189](https://gitlab.com/znixian/OpenOVR/-/merge_requests/189), Faye Salwin)
- Half-Life: Alyx: `IVRIPCResourceManagerClient` is answered as not found instead of stopping the game, and
  `VRControlPanel()` is exported. ([!190](https://gitlab.com/znixian/OpenOVR/-/merge_requests/190), arealjim)
- Action manifests with eye-tracking actions are read, skipping those actions, instead of aborting. So are long action
  names a game uses before it has asked for their handle.

**Input**
- Dpad bindings work in every action set, not only the first one synced (No Man's Sky's snap turning), and a
  double-click binding counts one press. ([!186](https://gitlab.com/znixian/OpenOVR/-/merge_requests/186), Faye Salwin)
- Valve Index trackpad dpad clicks: several click bindings on one trackpad no longer abort, and their thresholds no
  longer overwrite the action's own.
- Index controller bindings on controllers without a force-sensing trackpad (Vive, WMR) no longer abort.
- Index grip actions are cleared when a game reloads its action manifest.
- Generic trackers work on runtimes without the full `XR_MNDX_xdev_space` extension, which is now checked before use.
  ([!184](https://gitlab.com/znixian/OpenOVR/-/merge_requests/184), KaiErikNiermann)

**Frame timing**
- `GetFrameTiming` reports measured frame times and dropped frames instead of constants, so games that adjust their
  resolution to the load (Half-Life: Alyx's auto-fidelity and others) can.
  ([!191](https://gitlab.com/znixian/OpenOVR/-/merge_requests/191), arealjim)
- F1 23 no longer crashes in `GetFrameTiming`; each game's frame timing struct is filled within its own size, including
  Proton's padded one. ([!114](https://gitlab.com/znixian/OpenOVR/-/merge_requests/114), Manuel Kraus)
- Older `IVRCompositor` versions (`_009` to `_016`) get a struct of their own size; Proton 9 and later overran
  `_016`'s by 4 bytes.

**Vulkan and Direct3D 11**
- Vulkan starts on devices without external memory, and accepts one GPU seen through several Vulkan instances by
  passing the runtime's own device. ([!187](https://gitlab.com/znixian/OpenOVR/-/merge_requests/187), Faye Salwin)
- Vulkan games whose queue isn't in family 0 get a command pool of their queue's family.
- `invertUsingShaders` handles typeless and array textures, and falls back to a copy for textures the shader can't
  read. ([!96](https://gitlab.com/znixian/OpenOVR/-/merge_requests/96), Zak Ahmed)
- The D3D11 copy uses the right subresource, and resolves multisampled (MSAA) textures.

**Overlays**
- Overlays get the right position, rotation, size and sort order, follow the device they are attached to (a wrist
  menu), hide when that device isn't tracked, and laser pointers hit them where they are drawn.
  ([!150](https://gitlab.com/znixian/OpenOVR/-/merge_requests/150), mittorn)
- Curved overlays, through `XR_KHR_composition_layer_cylinder`, drawn flat where the runtime lacks it.
  ([!130](https://gitlab.com/znixian/OpenOVR/-/merge_requests/130), galister)
- With `laserMouse`, the controllers point at overlays that take mouse input as a mouse: a cursor where the laser
  meets one, clicks with the trigger, scrolling with the thumbstick, and laser haptics.
- With `ownFrames`, message boxes (`ShowMessageOverlay`) are shown in front of the player, in frames of OpenLamina's
  own, and answered with the laser.
- With `keyboard`, the keyboard games show for typing text is back, on every graphics API, in a US or UK layout
  (`keyboardLayout`), typed on with the laser.
- With `steamvrRenderModels`, controllers and trackers have SteamVR's own models, read from your SteamVR install.
- Overlay applications (`VRApplication_Overlay`) and background applications (`VRApplication_Background`) run on
  runtimes with overlay and headless sessions (`XR_EXTX_overlay`, `XR_MND_headless`, as Monado has): an overlay
  application's overlays are shown over the game, and a background application reads poses and events while another
  OpenLamina program runs. On Linux a background application gets a scene session.
- A dashboard of OpenLamina's own opens over the game with a controller's system button (`dashboardButton`): the
  dashboard overlays of the overlay applications running beside it, a tab for each, and a button to quit the game.
  Holding the system button for a second recentres instead, as in SteamVR.
- A render scale another program sets (`supersampleScale`, the game's `resolutionScale`) reaches the game at its next
  frame, with the event SteamVR sends for it, so a game that asks for the recommended size again renders at the new one.

**Stability and building**
- The log opens before anything writes to it (upstream issue #479), and a log that can't be opened no longer crashes
  the game. ([!185](https://gitlab.com/znixian/OpenOVR/-/merge_requests/185), Patrick Nicolas)
- Null pointers, zero frame counts and buffer sizes are checked where games can pass them.
- Builds with current MSVC (`<chrono>`: [!188](https://gitlab.com/znixian/OpenOVR/-/merge_requests/188), Faye
  Salwin), with clang, with optimised gcc and with Python 3.12 and later. Linux arm64, Android and macOS builds go to
  their own output folders. ([!179](https://gitlab.com/znixian/OpenOVR/-/merge_requests/179), Kitlith)

**Releases**
- [Windows and Linux builds](https://github.com/feimos-dev/OpenLamina/releases), 64- and 32-bit, built by GitHub
  Actions from the tagged commit, with checksums, the licences and the full source. Upstream's download links and
  launcher fetch upstream's builds.

Every change was reviewed, and all four builds are made with warnings as errors on every change. Not every change has
been tried yet in the game it is for. If something works upstream and not here, please
[open an issue](https://github.com/feimos-dev/OpenLamina/issues/new).

Not here yet: overlay apps (upstream !101).

## What OpenLamina is

OpenLamina is an implementation of OpenVR, SteamVR's API, that forwards a game's calls straight to the OpenXR
runtime: Meta's, Monado, Windows Mixed Reality, Pimax's, Varjo's and others. Think of it as a backwards
[ReVive](https://github.com/LibreVR/Revive). Games run on your headset as though they were native OpenXR games,
without SteamVR in between. Reasons to use it:

* Some machines have trouble running SteamVR.
* SteamVR can take a while to start, and is several gigabytes.
* OpenXR runtimes can be less prone to the stutter some people get in SteamVR.

OpenComposite, which this was forked from, used to be called OpenOVR ("OpenVR for OculusVR") and was renamed to avoid
confusion with OpenVR; the source folders here still carry that name.

Two community resources written for upstream mostly apply here too: mbucchia's
[FAQ](https://mbucchia.github.io/OpenXR-Toolkit/opencomposite.html) and the
[game compatibility spreadsheet](https://docs.google.com/spreadsheets/d/1s2SSuRt0oHm91RUZB-R-ef5BrfOtvP_jwDwb6FuMF9Q/edit#gid=2068512515).
A game listed there as broken by a missing interface will likely work with this fork, make an issue if not!

## Installing

Your headset's OpenXR runtime must be the active one: OpenLamina runs OpenVR games on it. Everything is on the
[releases page](https://github.com/feimos-dev/OpenLamina/releases).

### Windows

1. Download `OpenLamina-Setup.exe` and run it. It installs for your user only, without an administrator, in
   `%LOCALAPPDATA%\Programs\OpenLamina`. It isn't signed yet, so Windows may say it "protected your PC": choose
   *More info*, then *Run anyway*. It needs the .NET Framework 4.8, which Windows 10 (1903 or later) and 11 come with.
2. Leave *Use OpenLamina for OpenVR games now* ticked on its last page. OpenVR games then load OpenLamina.

The **OpenLamina** app (Start menu) shows what OpenVR games use and the active OpenXR runtime, and has:

- **Use OpenLamina** and **Use SteamVR**. Starting SteamVR from Steam also switches back to SteamVR, so after
  using SteamVR, open the app and press *Use OpenLamina* again.
- **Per-app settings**: send one program to SteamVR (or to OpenLamina) whatever the switch says. Programs appear in
  the list once they have tried to start VR with OpenLamina.
- **Settings**: the options of `openlamina.ini` you're most likely to change (overlay tools, input, picture and game
  behaviour), each with its name in the file and its default. They are saved in the `openlamina.ini` next to the
  libraries and apply to games started afterwards. The rest are set in the file by hand, see
  [Configuration file](#configuration-file).
- **Updates**: when opened, it looks for a newer release here and offers *Update to …*, which downloads the new
  installer, checks its SHA-256 against the one GitHub lists, and runs it. Nothing updates by itself. *Include
  pre-releases* offers those too.

To uninstall, use Windows' *Installed apps*. The uninstaller first takes OpenLamina out of
`openvrpaths.vrpath`, putting SteamVR first again if OpenLamina was. It won't run while a game uses OpenLamina.

For scripts: `OpenLamina-Setup.exe /VERYSILENT /SWITCH` installs and switches without asking anything (leave out
`/SWITCH` to only install). `OpenLamina.exe --use-openlamina`, `--use-steamvr` and `--remove` switch without
opening the window.

### Linux

```bash
tar -xzf OpenLamina-linux.tar.gz
OpenLamina/install.sh
```

`install.sh` copies OpenLamina to `~/.local/share/OpenLamina`, then puts that folder first in `"runtime"` of
`~/.config/openvr/openvrpaths.vrpath`, keeping the other entries after it (the old file is kept as
`openvrpaths.vrpath.bak`). It doesn't need root, but it needs `python3`. At the end it prints the Steam launch options
your OpenXR runtime needs inside the Steam Runtime (see [Linux-specific info](#linux-specific-info)). Afterwards, run
the installed copy:

```bash
~/.local/share/OpenLamina/install.sh --status          # what OpenVR games use, what's installed
~/.local/share/OpenLamina/install.sh --steamvr         # back to SteamVR (starting SteamVR from Steam does it too)
~/.local/share/OpenLamina/install.sh --openlamina   # and to OpenLamina again
~/.local/share/OpenLamina/install.sh --update          # the newest release, if newer (--pre: pre-releases too)
~/.local/share/OpenLamina/install.sh --launch-options  # the Steam launch options again
~/.local/share/OpenLamina/install.sh --uninstall
```

`--update` downloads the release's archive and checks its SHA-256 against the one GitHub lists before installing it.
Files you put in `bin/` (`openlamina.ini`, `apps-config.json`) survive updates.

**Flatpak Steam** has its own `openvrpaths.vrpath` and can't see `~/.local/share`. With only Flatpak Steam installed,
`install.sh` uses its folders by itself (`~/.var/app/com.valvesoftware.Steam/.local/share/OpenLamina` and
`.../.config/openvr/openvrpaths.vrpath`); with both kinds, add `--flatpak` to every command for Flatpak Steam.

### By hand

Unpack `OpenLamina-windows.zip` or `OpenLamina-linux.tar.gz` somewhere it can stay. Each holds an
`OpenLamina` folder with the 64- and 32-bit libraries in `bin`. Put that folder first in `"runtime"` of
`openvrpaths.vrpath` (`%LOCALAPPDATA%\openvr\openvrpaths.vrpath` on Windows, `~/.config/openvr/openvrpaths.vrpath` on
Linux), keeping the other entries after it. To switch back, put SteamVR's folder first again.

**One game only, on Windows:** back up the game's `openvr_api.dll`, then put `vrclient_x64.dll` (or `vrclient.dll` for a
32-bit game) in its place, renamed to `openvr_api.dll`. Steam's *Verify integrity of game files* undoes this. The
installer never changes game folders.

### Linux-specific info

As most VR games are Windows games run through Proton, you'll usually install system-wide, which `install.sh` does
(above). An environment variable lets you choose per game instead. By hand, unpack the tarball, which gives an
`OpenLamina` folder, and then:

1. Get the runtime in place, by one of these:
	- Set `VR_OVERRIDE` to the `OpenLamina` folder, in the game's Steam launch options:
	  `VR_OVERRIDE=/path/to/OpenLamina %command%`
	- Edit `~/.config/openvr/openvrpaths.vrpath`, and put the `OpenLamina` folder first in `"runtime"`. Steam likes
	  to rewrite this file, so you may need to make it read-only.
	- For the rare native Linux game, copy `bin/linux64/vrclient.so` over the game's `libopenvr_api.so`.
2. Games in the Steam Runtime (most of them) need a few paths made visible inside it:
	- Files in your home directory are there as they are.
	- Paths under `/usr` are under `/run/host/usr`. For example, a system-wide Monado's runtime JSON is
	  `/run/host/usr/share/openxr/1/openxr_monado.json`.
	- Other paths can be added with the `PRESSURE_VESSEL_FILESYSTEMS_RW` environment variable.

For example, with OpenLamina in your home directory, `VR_OVERRIDE`, and Monado installed system-wide, the launch
options might be:

```
VR_OVERRIDE=~/OpenLamina XR_RUNTIME_JSON=/run/host/usr/share/openxr/1/openxr_monado.json PRESSURE_VESSEL_FILESYSTEMS_RW=$XDG_RUNTIME_DIR/monado_comp_ipc %command%
```

`$XDG_RUNTIME_DIR/monado_comp_ipc` is Monado's socket: the game needs to see it to find Monado running. More on
`PRESSURE_VESSEL_FILESYSTEMS_RW` and the Steam Runtime's other variables is in
[its documentation](https://gitlab.steamos.cloud/steamrt/steam-runtime-tools/-/blob/master/pressure-vessel/wrap.1.md).

The Linux builds need glibc 2.38 or later (Ubuntu 24.04, Debian 13, Fedora 39 or newer).

## Coming from OpenComposite

What you set up for OpenComposite keeps working, and nothing of it is changed or deleted:

- `opencomposite.ini` is read wherever `openlamina.ini` would be. If a folder has both, only `openlamina.ini` is read.
- Binding files in `%LOCALAPPDATA%\OpenComposite\bindings` (Linux: `~/.local/state/OpenComposite/bindings`) and in a game's
  `OpenComposite` folder are found where they are, after the `OpenLamina` ones.
- The shared settings, room setup and application list (`%LOCALAPPDATA%\OpenComposite\store`) are copied once to
  `%LOCALAPPDATA%\OpenLamina\store` the first time OpenLamina runs, and OpenLamina uses its own copy from then on.
- `OPENCOMPOSITE_DEBUG_BREAK` and `OPENCOMPOSITE_TRACKER_SERIALS` are read when the `OPENLAMINA_` ones aren't set.
- `install.sh --opencomposite` still works.

The Windows installer is a different app from OpenComposite's and installs next to it, in
`%LOCALAPPDATA%\Programs\OpenLamina`. Uninstall OpenComposite from Windows' *Installed apps* when you no longer want it.

## Configuration file

On startup, OpenLamina reads a file named `openlamina.ini` (or `opencomposite.ini`, as it was called; where both are in a
folder, only `openlamina.ini` is read) from two places, in this order: next to the `openvr_api.dll` file (Windows only; on
Linux this one is not read), then the working directory (usually the folder of the EXE). Both are read when both exist,
and an option set in the working directory's file replaces the same option from the first, one option at a time. If no
file is found, that's fine and all the default values are used.

On Windows, the OpenLamina app's *Settings* window changes the common options in the first of these files, and leaves
everything else in it (comments, other options) as it is.

This is its configuration file, where you can specify a number of options. These are listed in `name=value` format, and you
can define a comment by starting a line with `;` or a `#`. Note the option names **are** case sensitive. An option OpenLamina
doesn't know, or a value it can't read, is logged as a warning (in the program's log) and that option keeps its default; the game
starts either way. Any whitespace between the name, equals sign, and value is ignored, as is whitespace on the end of the line.
Ensure your lines do not begin with whitespace however. Options go before any `[section]` heading or under `[default]`;
an option under any other heading is logged as ignored.

The available options are:

* `renderCustomHands` - boolean, default `enabled`. 
	* Should OpenLamina render custom hands. Disable this if you *really* dislike the hand models, and some games (like Skyrim) come with backup models that will be used instead.
* `handColour` - colour, default `#4c4c4c`. 
	* The colour of the hands OpenLamina draws, as they currently lack proper textures.
* `supersampleRatio` - float, default `1.0`. 
	* The supersample ratio in use - this is similar to what you would enter into SteamVR, a value of `145%` in SteamVR is a value of `1.45` here. A value of `80%` in SteamVR is `0.8` here, and so on. Higher numbers improve graphics, at a major performance cost.
* `haptics` - boolean, default `enabled`. 
	* Should haptic feedback to the controllers be enabled.
* `eyeTracking` - boolean, default `enabled`.
	* Give games the eye gaze (OpenXR's `XR_EXT_eye_gaze_interaction`, where the runtime and headset have it), for games with eye-tracked foveated rendering, through IVRSystem's foveation calls and IVRInput's eye tracking actions. It only does anything on a headset with an eye tracker. Disable it to not ask the runtime for the gaze; such games then use their fixed foveation.
* `admitUnknownProps` - boolean, default `disabled`. 
	* If asked for a tracked device property it does not understand, should OpenLamina ignore it.
* `logGetTrackedProperty` - boolean, default `disabled`. 
	* Print logging information when the app requests information about tracked devices, such as the HMD or the Touch controllers. On some games, this causes a log entry to be generated every frame, which isn't great for performance and clutters up the log. This is potentially useful for troubleshooting, and was enabled by default before the config option existed. In general, unless you've been told to enable this (or you know what you're doing while troubleshooting) you don't need to enable this.
* `stopOnSoftAbort` - boolean, default `disabled`.
    * Ends the game at the first call OpenLamina can't answer properly, as it used to. By default such a call gets an error or an empty answer and the game goes on; each one is logged the first time, and when the game shuts OpenVR down the log ends with a compatibility summary (`OpenLamina compatibility summary: <n> unsupported calls in <m> places`, then each place with how often it was hit). That list is what's worth reporting when a game misbehaves. Only enable this to find where a game first goes wrong.
* `dx10Mode` - ignored: Direct3D 10 textures are recognised by their interface.
* `enableHiddenMeshFix` - boolean, default `enabled`. 
	* Alter the coordinates of the hidden area mesh mask to fit within the view's projection. The hidden area mesh mask defines a region to which the game will not draw and is used to mask off areas of the display that you typically cannot see in the headset. If you see odd black regions around the view then try disabling this fix.
* `invertUsingShaders` - boolean, default `disabled`. 
	* Copy every frame through the shader pass (D3D11, D3D12) or a blit (Vulkan, OpenGL), even when its bounds don't flip it. Textures whose bounds flip them (games that render upside down and rely on the runtime to turn them over) are flipped in the copy either way. This might have a minor cost in performance.
* `initUsingVulkan` - boolean, default `disabled` on Windows, and `enabled` in builds without Direct3D 11. 
	* If available the temporary graphics adapter at start up will use Vulkan by default. This may be incompatible with some games. This will mostly affect Oculus headsets. 
* `hiddenMeshVerticalScale` - float, default `1.0`. 
	* The scaling factor used for the hidden area mesh if supported by the application. The hidden area mesh is a region that the game doesn't render to. If you set this lower e.g. `0.8` then less will be drawn at the very top and very bottom of the image improving performance. Suggested range is `0.5` to `1.0`.
* `useSubmittedPose` - boolean, default `true`.
	* For games that submit the head pose they rendered with (`Submit_TextureWithPose`): each eye's view is placed by that pose, rather than by the one OpenLamina located for the frame.
* `submitDepth` - boolean, default `true`.
	* For games that submit depth (`Submit_TextureWithDepth`), on runtimes with `XR_KHR_composition_layer_depth`: the depth goes to the runtime with each view (for reprojection), with the near and far planes of the game's projection. A Direct3D 12 game's depth texture is taken to be in `D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE` when it's submitted, as its colour texture is, and is left in it.
* `ownFrames` - boolean, default `true`.
	* While the game runs no frames (loading, a stall) and has an overlay or a skybox to show, OpenLamina runs frames of its own: the game's last frame again, which the runtime keeps reprojecting, with the overlays on top, or the skybox (`SetSkyboxOverride`) as equirect layers in its place. They start 100 ms after the game's last `WaitGetPoses` and never while its frame is open. On a Direct3D 11 session the device's immediate context is made multithread protected (logged), since these frames end on a thread of their own; a Vulkan or OpenGL session runs none, since its queue or context is the game's (Vulkan and OpenGL games whose frames go through Direct3D 11 have them). Without it, a skybox is shown at once as a quad, and the runtime is left with the game's last frame.
* `mirrorWindow` - boolean, default `false`.
	* A window on the desktop with the game's left eye, for games that ask for one (`ShowMirrorWindow`); closing it hides it. Its image is read back from the game's texture at full size, about 30 times a second, which costs frame time on a headset of high resolution. Without it the call does nothing, and the OpenXR runtime may have a mirror of its own. Windows only.
* `keyboard` - boolean, default `true`.
	* The keyboard a game asks for (`ShowKeyboard`), typed by pointing at it with the controllers. The game still gets the controllers' input while it's up. Without it, the keyboard is answered as done at once.
* `keyboardLayout` - `us` or `uk`, default by the user's locale (`uk` for en-GB, `us` otherwise).
	* The layout of the keyboard games show to type text on, with the controllers' lasers.
* `laserMouse` - boolean, default `true`.
	* The controllers point at overlays that take mouse input (`VROverlayInputMethod_Mouse`) as a mouse: focus and mouse events, clicks with the trigger, a cursor where the laser meets the overlay, and laser haptics. Without it, such an overlay gets no events from the controllers. The keyboard and message boxes are pointed at with the lasers regardless (with `keyboard` and `ownFrames`). The game's lasers also reach the mouse overlays of overlay applications running beside it (`helperApps`), which then don't point at them themselves (their own keyboard they still do). Where a game's own mouse overlay and another program's are both under the laser, both get the click.
* `steamvrRenderModels` - boolean, default `true`.
	* Controller and tracker models from the user's SteamVR install, with their moving parts. Without it, OpenLamina's built-in hands and tracker.
* `helperApps` - boolean, default `true`.
	* Overlay and background applications get sessions of their own where the OpenXR runtime has overlay sessions (`XR_EXTX_overlay`) and headless sessions (`XR_MND_headless`). An overlay application's session is on a D3D11 device of OpenLamina's own, on the runtime's adapter, with frames OpenLamina runs for it; its textures are copied onto that device: Direct3D 11 and 12 textures through shared textures, OpenGL ones through `WGL_NV_DX_interop2` and Vulkan ones through `VK_KHR_external_memory_win32` (which the application's device must have enabled). Direct3D 10 textures, textures on another graphics adapter than the runtime's, and every texture on a runtime without D3D11 (Linux) are refused (`VROverlayError_InvalidTexture`, logged); images from memory (`SetOverlayRaw`, `SetOverlayFromFile`) are shown everywhere. `GetOverlayTexture` hands an overlay application a shared D3D11 texture with the overlay's contents, which it may then write in place (flushing its context) without another `SetOverlayTexture`; that texture's DXGI shared handle, or any other's, set on another overlay (`TextureType_DXGISharedHandle`) shows the same texture there, read every frame. An overlay gets `VREvent_OverlayShown` and `VREvent_OverlayHidden` when it's shown and hidden, a dashboard overlay when it becomes the one the dashboard shows. `FindOverlay` also finds the overlays of the other programs running beside the game, by their keys: their visibility, key, name, width, flags, placement and intersection can be read through that handle, and nothing changed. A background application joins only while another OpenLamina program runs (a game, or an overlay application), as it would join SteamVR only while SteamVR runs, so it never starts the runtime; until then it gets 121 `NoServerForBackgroundApp`. A native OpenXR game alone doesn't count. Its session has no graphics; it reads poses at the time now where the runtime converts the system's clock (`XR_KHR_win32_convert_performance_counter_time`, `XR_KHR_convert_timespec_time`). On Linux a background application gets a scene session. Without those extensions, with this off, or when the runtime fails to start, `VR_Init` refuses them (130 `InvalidApplicationType`, 121 `NoServerForBackgroundApp`).
* `overlayLayersPlacement` - whole number, default `1`.
	* Where an overlay application's layers go among other programs' (`XrSessionCreateInfoOverlayEXTX::sessionLayersPlacement`): higher numbers are drawn over lower ones.
* `dashboardButton` - `system`, `system_long`, `menu`, `menu_long` or `off`, default `system`.
	* What opens OpenLamina's dashboard over the game: either controller's system button pressed (`system`) or held for 0.6 s (`system_long`), as SteamVR's opens, on controllers that have one (Index, Vive wands) and runtimes that hand it to applications (Monado does); or the left controller's menu button pressed (`menu`) or held for 0.6 s (`menu_long`), which on Index controllers is the B button. It shows the dashboard overlays (`CreateDashboardOverlay`) of the overlay applications running beside the game in a panel ahead of you, a tab for each and a "Quit game" button, pointed at with the lasers. While it's open the game gets `VREvent_DashboardActivated`, its input is held (its actions read inactive), and the same button closes it again. With `off` there is no dashboard.
	* With `system`, the dashboard opens or closes when the button is let go, and holding it for 1 s recentres instead, with a short pulse on that controller: the origin goes under the headset and forward becomes the way it faces (turned about the vertical only; the floor stays where it is). On Monado, with its `libmonado` and `applyZeroPoses`, that moves Monado's stage on top of the offset it has, so the standing space moves for every program using Monado until Monado restarts, and OpenLamina programs get `VREvent_ChaperoneUniverseHasChanged` and `VREvent_ChaperoneDataHasChanged`; the boundary `drawBounds` draws (the runtime's stage bounds, or a room setup's play area) moves with it. The seated origin is reset as well (`VREvent_SeatedZeroPoseReset`). On other runtimes, and without `applyZeroPoses`, only the seated origin is reset and the standing space stays. The other values never recentre.
* `drawBounds` - boolean, default `true`.
	* A grid wall on the play area's edge (the stage's bounds the runtime reports, else the room setup's play area, as `IVRChaperoneSetup` commits it) when the HMD or a controller comes within 0.4 m of it, fading in as it comes closer. For runtimes that show no boundary of their own.
* `sessionEvents` - boolean, default `true`.
	* Send the events SteamVR sends when the game loses input focus to the runtime's menu or gets it back (`VREvent_InputFocusCaptured`, `VREvent_InputFocusReleased`), and when the headset is put on or taken off, on runtimes with `XR_EXT_user_presence` (the proximity sensor's `VREvent_ButtonPress` and `VREvent_ButtonUnpress`, `VREvent_TrackedDeviceUserInteractionStarted` and `VREvent_TrackedDeviceUserInteractionEnded`). The headset's standby while its session is stopped (`VREvent_EnterStandbyMode`, `VREvent_LeaveStandbyMode`). Also the IPD's changes (`VREvent_IpdChanged`) and `Prop_ContainsProximitySensor_Bool` from the runtime. Some games pause on them. The proximity sensor's state is given either way.
* `applyZeroPoses` - boolean, default `true`.
	* On Monado, with its `libmonado`: a room setup's standing zero pose that a program commits (`IVRChaperoneSetup`'s `SetWorkingStandingZeroPoseToRawTrackingPose`, as playspace movers and recentring tools do) moves Monado's stage. That moves it for every program using Monado, native OpenXR ones too, until Monado restarts. Without it, the zero pose is kept and reported, and nothing moves.
* `logAllOpenVRCalls` - boolean, default `false`
	* Log every OpenVR call a game makes. Similar to `logGetTrackedProperty`, this clutters logs and should not be enabled unless necessary.
* `inputEvaluator` - ignored.
	* Binding files are always evaluated by OpenLamina. An `openlamina.ini` that sets this key (`direct` or `steamvr`) still loads, and the log says the key does nothing.
* `bindingEditor` - string, default empty.
	* The program a game's "open binding UI" request starts.

The possible types are as follows:

* boolean - one of `on`, `true`, `enabled` to enable the setting, or `off`, `false` or `disabled` to do the opposite. These
are not case-sensitive, so `Enabled` or `tRUe` work fine.
* colour - same as the format used in HTML/CSS, it's a `#` followed by six hex characters. Mozilla has a
[tool](https://developer.mozilla.org/en-US/docs/Web/CSS/CSS_Colors/Color_picker_tool) to help with this. Again, it's case-insensitive.
* float - a floating point (decimal) number above 0, eg `123.456`. An empty value, zero or a negative number is refused.
* whole number - digits only, `0` or more, eg `1`.
* string - the text after the `=`, as it is. `dashboardButton` and `keyboardLayout` take only the words
listed with them (not case-sensitive); `bindingEditor` takes any text.

A value that isn't of its option's type is logged as a warning and the option keeps its default.

Example files:

- Running SkyrimVR with yellow hands, and haptic feedback disabled:

```
handColour=#ffff00
haptics = off
```

## Reporting a bug

If a game crashes, shows a missing interface, or behaves differently than under SteamVR, please
[open an issue here](https://github.com/feimos-dev/OpenLamina/issues/new). Say:

- the game, your headset and OpenXR runtime, and which release of this fork (or commit) you use;
- what happened and when: at start, after loading a level, on pressing a button;
- whether it also happens with upstream's build, if you know.

And attach the game's log, which has most of what's needed to find the cause. Each program has its own, named after its EXE: `openlamina.<program>.log` (`openlamina.hlvr.log` for `hlvr.exe`), written anew each time the program starts. It is in:

- Windows: `%LOCALAPPDATA%\OpenLamina\logs` (paste that into Explorer's address bar)
- Linux: `$XDG_STATE_HOME/OpenLamina/logs`, or `~/.local/state/OpenLamina/logs` if `XDG_STATE_HOME` isn't set

If neither exists, look for it next to the game's EXE.

## Building

The source is GPL-3.0-or-later. Clone with its submodules:

```bash
git clone --recursive https://github.com/feimos-dev/OpenLamina.git
```

It's a standard CMake project, so IDEs such as CLion and Visual Studio can build it too. The scripts below build it
the way the releases are built: Release, warnings as errors (`-DERROR_ON_WARNING=ON`). Pull requests are built the same
way on all four targets, so please make sure yours builds without warnings.

**Windows:** Visual Studio 2022 or newer, or its Build Tools, with the C++ x64/x86 tools and "C++ ATL"; Python 3; and
the Vulkan headers and loader, for example from vcpkg (`vcpkg install vulkan-headers vulkan-loader --triplet
x64-windows`). Then, in PowerShell 7:

```powershell
scripts\ci\build-windows.ps1 -Arch x64 -Vulkan C:\vcpkg\installed\x64-windows
```

This makes `build\windows-x64\bin\vrclient_x64.dll` (`-Arch x86` makes `vrclient.dll`). The script copies the Vulkan
files to `libs\vulkan`, where `CMakeLists.txt` looks for them; to build without Vulkan, remove `SUPPORT_VK` from the
preprocessor definitions.

**Linux:** the packages in `scripts/ci/ubuntu.dockerfile` (gcc with multilib, CMake, the autotools, Python 3, and the
GL and Vulkan development files), then:

```bash
scripts/ci/build-linux.sh x64
```

This makes `build/linux-x64/bin/linux64/vrclient.so` (`x86` makes `bin/vrclient.so`). A plain `cmake -B build && cmake
--build build` works too, and the build folder can then be used directly as `VR_OVERRIDE`.

**The installers:** `RuntimeSwitcher/` is the Windows app (`OpenLamina.exe`, C# on the .NET Framework 4.8, built
with the .NET SDK on any OS), revived from upstream's switcher and pointed at this fork's releases.
`installer/OpenLamina.iss` is the Windows installer and `installer/install.sh` the Linux one. With the two Windows
DLLs in `artifacts\windows-x64` and `artifacts\windows-x86`, the .NET SDK 8 or later and Inno Setup 6:

```powershell
scripts\ci\build-installer.ps1 -Artifacts artifacts -Version 2026.11.06
```

This runs `RuntimeSwitcher/Tests` (`dotnet test RuntimeSwitcher/Tests` alone works on Linux too) and makes
`build\installer\OpenLamina-Setup.exe`. `scripts/ci/test-install.sh OpenLamina-linux.tar.gz` tries `install.sh`
on a scratch home folder.

### How the code is generated

OpenLamina relies heavily on scripts that generate code; without them the repository would have roughly three to four
times as much. There are two:

1. One splits OpenVR's headers into one header per version of each interface, so the project can use many versions of
   the same interface.
2. One generates the interface stubs. Each interface has one implementation (`BaseSystem`, `BaseCompositor` and so
   on), and generated stub classes implement a binary-compatible version of each version of it (`GVRSystem_017`,
   `GVRSystem_015`, `GVRCompositor_022`), with C-like `FnTable`s for them (for other languages, notably C#).

Both run as part of the build. Adding or removing an OpenVR header makes CMake rebuild the project; otherwise only a
couple of files are rebuilt. The `doc/` folder says more about why the scripts exist and how they work.

### Adding a new OpenVR SDK

`tests/conformance/interfaces.txt` lists every interface version Valve has shipped, and `scripts/openvr_inventory.py`
keeps it up to date. When Valve tags a new SDK:

1. Clone Valve's repository without its files: `git clone --filter=blob:none --no-checkout https://github.com/ValveSoftware/openvr.git <dir>`.
2. Refresh the list: `python scripts/openvr_inventory.py --valve-repo <dir> --write tests/conformance/interfaces.txt`.
3. Add the new SDK's `headers/openvr.h` to `OpenVRHeaders/` as `openvr-<version>.h`, byte for byte.
4. Add a `GEN_INTERFACE` line in the interface's `OpenOVR/Reimpl/CVR*.cpp` file for each new version, and write the
   `Base*` methods it needs.
5. Check: `python scripts/openvr_inventory.py --check` lists every version that isn't offered yet. A version answered
   "not found" on purpose goes in `tests/conformance/not_implemented.txt` with its reason.

## Licence

OpenLamina itself is Free Software under the GPLv3:

```
This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program. If not, see <https://www.gnu.org/licenses/>.
```

[GPL Full Text](https://www.gnu.org/licenses/gpl-3.0-standalone.html). This also can be found in
the `LICENSE.txt` file supplied with the source code.

This fork is a modified version of [upstream OpenComposite](https://gitlab.com/znixian/OpenOVR): its changes since
upstream's commit `cff07db` are listed above and in this repository's history.

### OpenXR SDK

The OpenXR SDK is included as a git submodule. It's under the Apache 2 licence, the full text of
which can be found under `libs/openxr-sdk/LICENSE`.

### OpenVR

This program (and its source code) includes several versions of the
[OpenVR](https://github.com/ValveSoftware/openvr) headers. These are under the
[MIT Licence](https://github.com/ValveSoftware/openvr/blob/master/LICENSE). This also can be
found in the `LICENSE_OPENVR.txt` file supplied with the source code.

```
Copyright (c) 2015, Valve Corporation
All rights reserved.
```

### INIH

This program uses the [inih](https://github.com/benhoyt/inih) INI parsing library by Ben Hoyt. This is available under
the MIT licence, a copy of which can be found in the `LICENSE_INIH.txt` file of this repository.

### Microblink Build

This program uses a part of Psiha's and Microblink's [build](https://github.com/microblink/build) CMake utilities
library. This is available under the two-clause BSD licence, a copy of which can be found in the `LICENCE_BUILD_UTIL.txt`
file of this repository.

### DebugBreak

This program uses Scott Tsai's [DebugBreak](https://github.com/scottt/debugbreak) library when built on Linux. Its full
text can be found in `LICENCE_DEBUG_BREAK.txt`.
