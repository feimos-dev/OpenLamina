#!/bin/sh
# The conformance runs of the Linux CI jobs (.github/workflows/build.yml), against a build made with
# scripts/ci/build-linux.sh --tests: the no-runtime run and the stand-in runtime's (tests/runtime), each compared with
# its Linux baseline (regress.py), what OpenLamina does when OpenXR calls fail, and the cases with exact answers
# (expect/) that apply on Linux. Needs a Vulkan driver (Mesa's lavapipe on CI) and an X server for the OpenGL cases
# (DISPLAY: xvfb-run on CI).
#
#   scripts/ci/conformance-linux.sh x64|x86 [build-dir] [out-dir]
#
# Exits non-zero if any run failed; every run is made all the same. Left out, as they don't apply here: the cases of
# Direct3D (their Vulkan and OpenGL counterparts run), OpenGL and Vulkan through D3D11, OpenLamina's own frames on a
# thread of their own when the game is idle (on D3D sessions only: Vulkan and OpenGL ones run them only inside the
# game's calls), the Steam client observer and the background apps' refusals (background apps get a scene session
# on Linux, for Proton).
set -u

root=$(cd "$(dirname "$0")/../.." && pwd)
arch=${1:-}
case $arch in
x64) lib=bin/linux64/vrclient.so overlay= ;;
x86) lib=bin/vrclient.so overlay=tests/conformance/baseline/stand-in-linux-x86.json ;;
*)
	echo "usage: $0 x64|x86 [build-dir] [out-dir]" >&2
	exit 2
	;;
esac
build=${2:-$root/build/linux-$arch}
out=${3:-$root/conformance}
cd "$root"

failed=0
check() {
	if ! "$@"; then
		failed=$((failed + 1))
		echo "::error::failed: $*"
	fi
}
run() {
	check python3 tests/conformance/run.py --runtime "$build/$lib" --conformance "$build/tests/oc_conformance" --xr-runtime stand-in --ci "$@"
}

# Nothing may end, crash or hang; every record is compared with the Linux baselines
check python3 tests/conformance/run.py --runtime "$build/$lib" --conformance "$build/tests/oc_conformance" --xr-runtime none --ci --out "$out"/none
check python3 tests/conformance/regress.py --results "$out"/none/results.json --baseline tests/conformance/baseline/none-linux.json
check python3 tests/conformance/run.py --runtime "$build/$lib" --conformance "$build/tests/oc_conformance" --xr-runtime none --ci --cases applications --cases-only --expect tests/conformance/expect/applications.json --out "$out"/applications-none
run --out "$out"/stand-in
check python3 tests/conformance/regress.py --results "$out"/stand-in/results.json --baseline tests/conformance/baseline/stand-in-linux.json ${overlay:+--baseline "$overlay"}
# What OpenLamina does when OpenXR calls fail (expect/xr_failures.json)
check python3 tests/conformance/xr_failures.py --runtime "$build/$lib" --conformance "$build/tests/oc_conformance" --ci --out "$out"/xr-failures
# Exact answers for chosen cases (expect/)
run --cases old-overlay --script tests/conformance/scripts/keyboard.json --ini tests/conformance/ini/keyboard_us.ini --expect tests/conformance/expect/old_overlay.json --out "$out"/old-overlay
run --cases overlay-transforms --script tests/conformance/scripts/overlay_transforms.json --expect tests/conformance/expect/overlay_transforms.json --out "$out"/overlay-transforms
run --cases old-misc --expect tests/conformance/expect/old_misc.json --out "$out"/old-misc
run --cases new-interfaces --expect tests/conformance/expect/new_interfaces.json --out "$out"/new-interfaces
run --cases device-identity --expect tests/conformance/expect/device_identity.json --out "$out"/device-identity
run --cases applications --expect tests/conformance/expect/applications.json --out "$out"/applications
run --cases chaperone,chaperone-read --jobs 1 --script tests/conformance/scripts/stage_2x3.json --expect tests/conformance/expect/chaperone.json --out "$out"/chaperone
run --cases events --script tests/conformance/scripts/legacy_click.json --expect tests/conformance/expect/events.json --out "$out"/events
run --cases events-listen,events-commit --jobs 2 --expect tests/conformance/expect/events_two_processes.json --out "$out"/events-two-processes
# The events SteamVR sends for session changes: with sessionEvents all of them, without it only the IPD's and the quit
run --cases session-events --script tests/conformance/scripts/session_events.json --ini tests/conformance/ini/session_events.ini --expect tests/conformance/expect/session_events.json --out "$out"/session-events
run --cases session-events --script tests/conformance/scripts/session_events.json --expect tests/conformance/expect/session_events_off.json --out "$out"/session-events-off
run --cases velocities --script tests/conformance/scripts/velocities.json --expect tests/conformance/expect/velocities.json --out "$out"/velocities
# The binding evaluator evaluates a toggle_button, and reads a vector3
run --cases input-frames --script tests/conformance/scripts/input_toggle.json --manifest tests/unit/data/manifest_toggle.json --expect tests/conformance/expect/input_evaluator.json --out "$out"/input-steamvr
# GetActionBindingInfo from the binding file (poses and vibrations included), and without one a vibration's bound outputs
run --cases binding-info --script tests/conformance/scripts/bound_source_odd.json --manifest tests/unit/data/manifest_toggle.json --expect tests/conformance/expect/binding_info.json --out "$out"/binding-info
run --cases binding-info --script tests/conformance/scripts/bound_source_odd.json --manifest tests/unit/data/manifest_haptic_no_bindings.json --expect tests/conformance/expect/binding_info_runtime.json --out "$out"/binding-info-runtime
# Vive trackers by their role: a device per role, and a pose action a tracker's binding file binds read from it
run --cases vive-trackers --script tests/conformance/scripts/vive_trackers.json --manifest tests/unit/data/manifest_trackers.json --expect tests/conformance/expect/vive_trackers.json --out "$out"/vive-trackers
# The hands as controllers: through XR_EXT_hand_interaction, with hand tracking alone, and shown as Touch controllers
run --cases hand-controllers --script tests/conformance/scripts/hand_interaction.json --manifest tests/unit/data/manifest_trackers.json --expect tests/conformance/expect/hand_controllers.json --out "$out"/hand-controllers
run --cases hand-controllers --script tests/conformance/scripts/hand_tracking_only.json --manifest tests/unit/data/manifest_trackers.json --expect tests/conformance/expect/hand_controllers_tracking.json --out "$out"/hand-controllers-tracking
run --cases hand-controllers --script tests/conformance/scripts/hand_interaction.json --manifest tests/unit/data/manifest_trackers.json --ini tests/conformance/ini/hand_controller_touch.ini --expect tests/conformance/expect/hand_controllers_touch.json --out "$out"/hand-controllers-touch
# The compositor's calls with every graphics API offered: the Vulkan extension lists with a null buffer
run --only "IVRCompositor_0(19|2[0-9])" --script tests/conformance/scripts/gfx_all_apis.json --out "$out"/compositor-all-apis
# Eye-tracked foveation from the eye gaze: with an action manifest and with legacy input, canted views,
# eyeTracking=off, no extension
run --cases eye-gaze --script tests/conformance/scripts/eye_gaze.json --manifest tests/unit/data/manifest_basic.json --expect tests/conformance/expect/eye_gaze.json --out "$out"/eye-gaze
run --cases eye-gaze --script tests/conformance/scripts/eye_gaze.json --expect tests/conformance/expect/eye_gaze.json --out "$out"/eye-gaze-legacy
run --cases eye-gaze --script tests/conformance/scripts/eye_gaze_canted.json --manifest tests/unit/data/manifest_basic.json --expect tests/conformance/expect/eye_gaze_canted.json --out "$out"/eye-gaze-canted
run --cases eye-gaze --script tests/conformance/scripts/eye_gaze.json --manifest tests/unit/data/manifest_basic.json --ini tests/conformance/ini/eye_tracking_off.ini --expect tests/conformance/expect/eye_gaze_off.json --out "$out"/eye-gaze-off
run --cases eye-gaze --manifest tests/unit/data/manifest_basic.json --expect tests/conformance/expect/eye_gaze_off.json --out "$out"/eye-gaze-noext
# IVRInput's eye tracking action: a manifest with a binding file naming it, a gaze without a position, eyeTracking=off
run --cases eye-tracking --script tests/conformance/scripts/eye_gaze.json --manifest tests/unit/data/manifest_eye_tracking.json --expect tests/conformance/expect/eye_tracking.json --out "$out"/eye-tracking
run --cases eye-tracking --script tests/conformance/scripts/eye_gaze_no_position.json --manifest tests/unit/data/manifest_eye_tracking.json --expect tests/conformance/expect/eye_tracking.json --out "$out"/eye-tracking-no-position
run --cases eye-tracking --script tests/conformance/scripts/eye_gaze.json --manifest tests/unit/data/manifest_eye_tracking.json --ini tests/conformance/ini/eye_tracking_off.ini --expect tests/conformance/expect/eye_tracking_off.json --out "$out"/eye-tracking-off
# Input from two threads at once
run --cases input-threads --repeat 5 --script tests/conformance/scripts/input_toggle.json --manifest tests/unit/data/manifest_toggle.json --expect tests/conformance/expect/input_threads.json --out "$out"/input-threads-steamvr
# The display's refresh rate
run --cases display-frequency --script tests/conformance/scripts/refresh_72_120.json --expect tests/conformance/expect/display_frequency_fb.json --out "$out"/display-frequency-fb
run --cases display-frequency --script tests/conformance/scripts/display_120.json --expect tests/conformance/expect/display_frequency_measured.json --out "$out"/display-frequency-measured
# Every interface as an overlay application on its session: nothing crashes, ends or hangs
run --apptypes 2 --script tests/conformance/scripts/helper_apps.json --out "$out"/helper-apps-interfaces
# An overlay app's Vulkan and OpenGL textures: shared with OpenLamina's own device (VK_KHR_external_memory_fd,
# GL_EXT_memory_object_fd, which lavapipe and llvmpipe have), and read back into memory where the program has neither.
# One case a run, so that its log says whether it was shared.
for api in vk gl; do
	run --cases overlay-app-$api --script tests/conformance/scripts/helper_apps.json --expect tests/conformance/expect/helper_apps_linux_$api.json --out "$out"/helper-apps-shared-$api
	check sh -c 'ls "$1"/logs/*.log >/dev/null && ! grep "isn.t shared with the session" "$1"/logs/*.log' - "$out"/helper-apps-shared-$api
	check env OC_CONFORMANCE_NO_EXTERNAL_MEMORY=1 MESA_EXTENSION_OVERRIDE=-GL_EXT_memory_object_fd \
		python3 tests/conformance/run.py --runtime "$build/$lib" --conformance "$build/tests/oc_conformance" --xr-runtime stand-in --ci \
		--cases overlay-app-$api --script tests/conformance/scripts/helper_apps.json --expect tests/conformance/expect/helper_apps_linux_$api.json --out "$out"/helper-apps-readback-$api
	check sh -c 'grep -q "isn.t shared with the session" "$1"/logs/*.log' - "$out"/helper-apps-readback-$api
done
# The game's pointer on an overlay app's overlay, and an overlay app that ends without shutting down (two programs each)
run --cases remote-pointer,remote-pointer-overlay --jobs 2 --script tests/conformance/scripts/remote_pointer.json --ini tests/conformance/ini/laser_mouse.ini --expect tests/conformance/expect/remote_pointer.json --out "$out"/remote-pointer
run --cases remote-pointer-peer-dies,remote-pointer-dying-overlay --jobs 2 --script tests/conformance/scripts/remote_pointer.json --ini tests/conformance/ini/laser_mouse.ini --expect tests/conformance/expect/remote_pointer_peer_dies.json --out "$out"/remote-pointer-peer-dies
# OpenLamina's dashboard over the game, with an overlay app's dashboard overlay (two programs)
run --cases dashboard,dashboard-overlay --jobs 2 --script tests/conformance/scripts/dashboard.json --ini tests/conformance/ini/dashboard.ini --manifest tests/unit/data/manifest_basic.json --expect tests/conformance/expect/dashboard.json --out "$out"/dashboard
# A controller's system button held recentres and opens no dashboard; pressed, it opens and closes the dashboard
run --cases recentre --script tests/conformance/scripts/recentre.json --ini tests/conformance/ini/recentre.ini --expect tests/conformance/expect/recentre.json --out "$out"/recentre
# Controllers found from the game's first frames
run --cases controllers-early --script tests/conformance/scripts/controllers_early.json --expect tests/conformance/expect/controllers_early.json --out "$out"/controllers-early
# The play area's walls near its edge with drawBounds, and none without
run --cases bounds --script tests/conformance/scripts/bounds.json --ini tests/conformance/ini/draw_bounds.ini --expect tests/conformance/expect/bounds.json --out "$out"/bounds
run --cases bounds-room-setup --script tests/conformance/scripts/bounds_room_setup.json --ini tests/conformance/ini/draw_bounds.ini --expect tests/conformance/expect/bounds_room_setup.json --out "$out"/bounds-room-setup
run --cases bounds --script tests/conformance/scripts/bounds.json --expect tests/conformance/expect/bounds_off.json --out "$out"/bounds-off
# The controllers' lasers on an overlay as a mouse
run --cases laser-mouse --script tests/conformance/scripts/laser.json --ini tests/conformance/ini/laser_mouse.ini --expect tests/conformance/expect/laser_mouse.json --out "$out"/laser-mouse
run --shipped-defaults --cases laser-mouse --script tests/conformance/scripts/laser.json --expect tests/conformance/expect/laser_mouse.json --out "$out"/defaults-laser-mouse
run --shipped-defaults --cases session-events --script tests/conformance/scripts/session_events.json --expect tests/conformance/expect/session_events.json --out "$out"/defaults-session-events
run --shipped-defaults --cases bounds --script tests/conformance/scripts/bounds.json --expect tests/conformance/expect/bounds.json --out "$out"/defaults-bounds
run --shipped-defaults --cases recentre --script tests/conformance/scripts/recentre.json --expect tests/conformance/expect/recentre.json --out "$out"/defaults-recentre
# A message box in OpenLamina's own frames, run inside the blocked call (no thread on a Vulkan or OpenGL session),
# answered with the laser; one the session ends under answers as the game's quit
run --cases message-overlay --script tests/conformance/scripts/message_box.json --ini tests/conformance/ini/own_frames.ini --expect tests/conformance/expect/message_overlay.json --out "$out"/message-overlay
run --cases message-overlay --script tests/conformance/scripts/message_box_exit.json --ini tests/conformance/ini/own_frames.ini --expect tests/conformance/expect/message_overlay_exit.json --out "$out"/message-overlay-exit
run --shipped-defaults --cases message-overlay --script tests/conformance/scripts/message_box.json --expect tests/conformance/expect/message_overlay.json --out "$out"/defaults-message-overlay
# A game that calls WaitGetPoses without Submit: its frames are ended for it inside the call
run --cases wait-without-submit --ini tests/conformance/ini/own_frames.ini --expect tests/conformance/expect/wait_without_submit.json --out "$out"/wait-without-submit
run --cases gfx-vk-wait-without-submit,gfx-gl-wait-without-submit --script tests/conformance/scripts/gfx_all_apis.json --ini tests/conformance/ini/own_frames.ini --expect tests/conformance/expect/gfx_wait_without_submit.json --out "$out"/gfx-wait-without-submit
# The keyboard, typed on with the laser
run --cases keyboard --script tests/conformance/scripts/keyboard.json --ini tests/conformance/ini/keyboard_us.ini --expect tests/conformance/expect/keyboard.json --out "$out"/keyboard
# Frames and overlays from the Vulkan and OpenGL games, as the stand-in reads them back (the Direct3D cases skip)
d3d='gfx-d3d1[012]-.*|overlay-(raw|texture-readback)-d3d1[12]|overlay-images'
run --cases overlay-images,gfx-d3d11-basic,gfx-d3d12-basic,gfx-vk-basic,gfx-gl-basic,gfx-d3d10-basic,gfx-d3d11-odd,gfx-d3d11-bounds,gfx-d3d12-bounds,gfx-vk-bounds,gfx-gl-bounds,overlay-raw-d3d11,overlay-raw-d3d12,overlay-raw-vk,overlay-raw-gl,overlay-texture-readback-d3d11,overlay-texture-readback-d3d12,overlay-texture-readback-vk,overlay-texture-readback-gl --script tests/conformance/scripts/gfx_all_apis.json --expect tests/conformance/expect/gfx.json --allow-skip "$d3d" --out "$out"/gfx
# CompositorDumpImages: the eyes' next images as PNGs in the log folder
run --cases gfx-vk-dump-images,gfx-gl-dump-images --script tests/conformance/scripts/gfx_all_apis.json --expect tests/conformance/expect/dump_images.json --out "$out"/dump-images
# A runtime with XR_KHR_vulkan_enable2 alone: the temporary device made through it, and the game's session on it
run --cases gfx-vk-basic,gfx-vk-bounds --script tests/conformance/scripts/gfx_vk_enable2.json --expect tests/conformance/expect/gfx_vk_enable2.json --out "$out"/gfx-vk-enable2
# Formats the runtime doesn't offer (it has RGBA8 sRGB only), converted into the one it has
run --cases gfx-d3d11-formats,gfx-d3d12-formats,gfx-vk-formats,gfx-gl-formats --script tests/conformance/scripts/gfx_rgba8_only.json --expect tests/conformance/expect/gfx_formats.json --allow-skip "$d3d" --out "$out"/gfx-formats
# The pose and depth a frame was submitted with: used with useSubmittedPose and submitDepth on, not without
run --cases gfx-d3d11-pose-depth,gfx-d3d12-pose-depth,gfx-vk-pose-depth,gfx-gl-pose-depth --script tests/conformance/scripts/gfx_depth.json --ini tests/conformance/ini/pose_depth.ini --expect tests/conformance/expect/gfx_pose_depth.json --allow-skip "$d3d" --out "$out"/gfx-pose-depth
run --cases gfx-d3d11-pose-depth,gfx-d3d12-pose-depth,gfx-vk-pose-depth,gfx-gl-pose-depth --script tests/conformance/scripts/gfx_depth.json --expect tests/conformance/expect/gfx_pose_depth_off.json --allow-skip "$d3d" --out "$out"/gfx-pose-depth-off
run --shipped-defaults --cases gfx-d3d11-pose-depth,gfx-d3d12-pose-depth,gfx-vk-pose-depth,gfx-gl-pose-depth --script tests/conformance/scripts/gfx_depth.json --expect tests/conformance/expect/gfx_pose_depth.json --allow-skip "$d3d" --out "$out"/defaults-pose-depth
# The cases whose answers wait on a SteamVR recording, with both controllers on (every record ok; the values
# are the baselines' and the recordings')
steamvr_run=app-type-answers,device-properties,init-again,chaperone-colours,submit-refusals,skeletal-input,legacy-controller,render-model-components,overlay-defaults,settings-defaults,application-list,io-buffer
run --cases "$steamvr_run" --script tests/conformance/scripts/controllers_on.json --out "$out"/steamvr-run
# An overlay app and a background app that submit no frames see the controllers through IVRSystem
run --cases helper-app-controllers --script tests/conformance/scripts/helper_apps_controllers.json --expect tests/conformance/expect/helper_app_controllers.json --out "$out"/helper-app-controllers
# The session made again under a game: for its action manifest in the middle of a frame, after a seated zero pose
# reset with its last frame still shown, and with a tracker it already knows
run --cases manifest-mid-frame --manifest tests/unit/data/manifest_basic.json --expect tests/conformance/expect/manifest_mid_frame.json --out "$out"/manifest-mid-frame
run --cases seated-zero-kept-frame --ini tests/conformance/ini/own_frames.ini --expect tests/conformance/expect/seated_zero_kept_frame.json --out "$out"/seated-zero-kept-frame
run --cases tracker-restart --script tests/conformance/scripts/xdev_tracker.json --expect tests/conformance/expect/tracker_restart.json --out "$out"/tracker-restart
run --cases role-tracker-restart --script tests/conformance/scripts/vive_trackers.json --manifest tests/unit/data/manifest_trackers.json --expect tests/conformance/expect/role_tracker_restart.json --out "$out"/role-tracker-restart

echo "$failed failed"
exit $((failed > 0))
