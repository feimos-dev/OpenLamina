# OpenVR conformance tests

`oc_conformance` loads a `vrclient` DLL, OpenLamina's or SteamVR's, and calls every method of every OpenVR
interface version Valve has shipped (`interfaces.txt`), with four argument sets (valid, edge, null, invalid), under
each application type. It also runs named cases that need real handles or frames (`cases.cpp`). `run.py` runs each
interface version, application type and argument set, and each case, in a child process, and collects what every call
returned and wrote.

Built with the tests (`-DOC_BUILD_TESTS=ON`, or `scripts\ci\build-windows.ps1 -Tests`,
`scripts/ci/build-linux.sh --tests`).

## Running it

```
python tests/conformance/run.py --runtime build/bin/vrclient_x64.dll --conformance build/tests/oc_conformance.exe --xr-runtime stand-in --out out
```

- `--xr-runtime stand-in` runs OpenLamina on `oc_test_runtime` (`tests/runtime`), which needs no headset and can
  be scripted with `--script` (examples in `scripts/`). `none` gives it no OpenXR runtime at all. A manifest path
  uses that runtime (Monado's, say). `monado:<manifest>` runs on a Monado service that is already running (the run
  stops at once if none is), and keeps the manifest with `--real-runtime`; there's no stand-in dump to join then.
- `--only <regex>` limits the interface versions, `--cases a,b` runs only those cases.
- `--repeat N` (at most 100) runs each case N times, each in its own child; every record gets its `round`, the summary
  says how many rounds of each case ended ok, and `--expect` checks every round. `--case-arg <name>=<value>` (repeatable)
  is passed to every case, as `input-threads`' `start_frame=<n>`.
- `--expected expected/steamvr-<version>.json --differences differences.json` compares every answer with SteamVR's
  recorded ones (`record-steamvr.ps1`); `--expect <file>` checks exact values for chosen records (`expect/`).
- `--known-failures <file>` lists records (keyed as in `--expect`) that may fail today; any other failure still fails
  the run, and a listed record that passes is named in the summary. `--allow-skip <regex>` lets the cases it matches
  pass `--expect` with a `skipped` record (an API this machine has no driver for).
- The frame cases (`gfx-<api>-<what>`, `gfx_cases.cpp`) submit a test pattern through each graphics API
  (`gfx/GfxApi.h`); a record with `"dump_join"` gets the stand-in runtime's dump joined in as `dump` (`join_dump` in
  `run.py`), which `--expect` fields named `dump:<path>` (`dump:views[0].samples.tl`) check. Samples are the bytes
  the swapchain image stores, as RGBA8 (`rrggbbaa`): an sRGB format's sRGB-encoded bytes, a UNORM one's linear
  bytes, with no conversion; the record's `format` says which. A sampled view that couldn't be read says why in
  `samples_error`. The dump keeps whole records for frames 1-64, every 32nd after and the last one
  (`tests/runtime/script.h`). An `--expect` value `{"near": v, "within": d}` takes a number within `d` of `v`, or a
  sample whose every channel is.
- Each child gets `OC_NO_MESSAGE_BOX=1` (OpenLamina ending a program exits with code 70, without a box),
  `OC_LOG_DIR` and `OC_STORE_DIR` under `--out`, so a run never touches the user's own logs or state. Each child has a
  store of its own (`store/<its output>`), so one child's settings and events never reach another, except the cases that
  work between programs (`SHARED_STORES` in `run.py`: events-listen with events-commit, chaperone with chaperone-read).
  Each child also gets `OC_TEST_NO_STEAMVR=1`, so OpenLamina finds no SteamVR install, except `render-models`, which
  gets `tests/unit/data/steamvr` (a fixture of our own, not Valve's files) as SteamVR's install through
  `OC_TEST_STEAMVR_DIR` (`CASE_ENV` in `run.py`).

The versions of `custom_interfaces.txt` (our own headers: `IVRPaths`, `IVRMailbox_001`, `IVRControlPanel_006`,
`IVRInputInternal_002`) are called like those of `interfaces.txt`. After an interface's own methods, the same methods
are called through its function table (`GetGenericInterface("FnTable:<version>")`); those records are named
`FnTable:<version>` and continue the interface's index. `coverage_cases.cpp` has the cases for what no generated caller
reaches: the C exports of `openvr_api` (`c-api-exports`), the client core's other methods and `IVRClientCore_002`
(`client-core-methods`), a held init token and interface pointer (`init-held`), the interfaces of each application type
(`app-type-interfaces`), `IVRMailbox_001` (`mailbox`), `IVRNotifications_002` (`notifications`) and the interfaces
that keep state across a Shutdown and Init, with SteamVR's resource files (`state-and-resources`).

`app-type-answers` starts each application type in turn (scene, overlay, background, utility, other) and records the
answers that depend on the session and the controllers: device roles and properties, the compositor's, overlays',
applications' and chaperone's state. Run it with both controllers on (`scripts/controllers_on.json` on the stand-in;
switched on with SteamVR), so a SteamVR recording gives the answers to compare with. `steamvr_run_cases.cpp` has more
of those, run the same way: device properties and roles, a second Init, chaperone bounds and colours, refused submits,
skeletal input (`manifest_skeleton.json`), legacy controller state, render model components, a new overlay's defaults
and events, settings defaults, the application list and IVRIOBuffer.

`out/summary.txt` has the counts per status (`ok`, `ended`: OpenLamina ended the program, `crashed`, `timeout`) and
every record that wasn't ok; `out/results.json` has every record. When a child ends early the runner starts it again
after the call that ended it, so one bad call doesn't hide the rest. When a child crashes, `crash_report.cpp` writes
why (the exception, abort or invalid CRT call) and its stack next to its output, and the runner puts that on the
record (`crash`) and its first line in the summary. A fail-fast inside the DLL (its own CRT's `abort` or `terminate`)
bypasses it: such a crash has no report.

## The SteamVR reference

`record-steamvr.ps1 -Conformance <oc_conformance.exe> -Out <dir>` runs everything against SteamVR with its null
driver (no headset needed) and writes `expected/steamvr-<version>.json`. It backs up the OpenVR paths file and
SteamVR's settings, room setup and app config first, puts every one back afterwards and fails unless their SHA-256s
match; `-WhatIf` only lists them. Close SteamVR, Monado and anything VR first.

`-Controllers` loads SteamVR's other drivers beside the null headset, so switched-on Lighthouse controllers take part
(for `app-type-answers` and `steamvr_run_cases.cpp`). Programs run one at a time (a program that brings SteamVR down
would take the others with it). A SteamVR whose server stops answering (each child then waits on its shared namespace
until the timeout) is ended after three timeouts in a row and started again; `results.json` counts these restarts
(`runtime_restarts`). `-Resume` keeps what an earlier run into the same `-Out` finished (each job's
`<raw>.result.json`) and runs the rest; `results.json` then has `"resumed": true`. Against a real runtime the methods
of `unsafe_methods.txt` aren't called (their records say `"skipped": "unsafe"`), among them `ShowMessageOverlay`, which
waits for a click in the headset: so no recording has SteamVR's answer to it.

`differences.json` lists the differences that are expected between SteamVR's null HMD and OpenLamina (display
geometry, timing and poses, device properties, handle numbers, the PC's own apps and settings, and the app types
OpenLamina doesn't support yet), each with its reason. Every other difference is a bug to fix or to explain there.
An entry matches `interface` and `method` (regexes) and `field`, and, if it has one, `apptype` (a regex of the app type
numbers). Records SteamVR answered while restarting between children (init errors 115, 117, 119) aren't compared.

An entry with `"arch": "x86"` applies only when `--runtime` is a 32-bit DLL: the reference was recorded by 64-bit
SteamVR, and answers that hold a pointer are compared as raw bytes, so they are shorter in a 32-bit run.

`scripts/openvr_inventory.py --check` (run by CI) fails unless every version in `interfaces.txt` (every one Valve has
shipped, app-facing or C API only) has a `GEN_INTERFACE` line or a reason in `not_implemented.txt`.

## Regression baselines

`regress.py --results <dir>/results.json --baseline baseline/<file>` fails when a record that was ok in the baseline
isn't any more, or when an answer changes (except the run-to-run fields in `volatile.json`); `--accept` writes a new
baseline, which keeps a hash of each answer and the answer itself where it is under 200 bytes, so a regression shows
the old and the new value (`--self-test` checks that). CI keeps `none.json` (no runtime) and `stand-in.json`. A later `--baseline` overrides the earlier ones per
record: `stand-in-x86.json` holds only the records a 32-bit run answers differently (the pointer-sized ones), and CI
passes it after `stand-in.json`. With several, `--accept` writes the last one, so after accepting `stand-in.json` from a
64-bit run, accept the 32-bit one with `--baseline baseline/stand-in.json --baseline baseline/stand-in-x86.json` from
a 32-bit build's run (or from `stand-in/results.json` in CI's `conformance-windows-x86` artifact).

## On Linux

The same runs work on Linux, with the stand-in runtime's Vulkan and OpenGL (Xlib and GLX) sessions:

```
python3 tests/conformance/run.py --runtime build/bin/linux64/vrclient.so --conformance build/tests/oc_conformance --xr-runtime stand-in --out out
```

Without a script the stand-in offers `XR_KHR_vulkan_enable` there (D3D11 on Windows): the API of OpenLamina's own
session. A script's D3D extensions are left out on Linux, and one left with no graphics API gets that one
(`tests/runtime/script.h`). The game's valid texture is a Vulkan image (a D3D11 one on Windows), and the frame cases'
Vulkan and OpenGL games run; their Direct3D cases skip. OpenLamina reads `openlamina.ini` only from the working
directory on Linux, so `run.py` runs the children in `work/` under `--out`, with the ini there. The OpenGL cases need
an X server (`xvfb-run` without a display), and the Vulkan ones a driver (Mesa's lavapipe will do).

`scripts/ci/conformance-linux.sh x64|x86 <build-dir> <out-dir>` makes the Linux CI jobs' runs: the no-runtime and
stand-in runs compared with `baseline/none-linux.json` and `baseline/stand-in-linux.json` (32-bit:
`stand-in-linux-x86.json` over it), `xr_failures.py`, and the `--expect` runs that apply on Linux. Their answers differ
from Windows' in places (structs packed as Linux's OpenVR headers pack them, GCC's enums, background applications
that get a scene session for Proton), so the Linux baselines are their own; accept them as on Windows, from a Linux
run (CI's `conformance-linux-x64` and `conformance-linux-x86` artifacts have them).

## Failed OpenXR calls

`xr_failures.py --runtime <dll> --conformance <exe> --out <dir>` runs the stand-in runtime with each script in
`scripts/` (a failing `xrCreateInstance`, no headset, a dropped frame, a lost or exiting session, a failed swapchain,
refused binding suggestions) and checks what `expect/xr_failures.json` says OpenLamina does for each: the init
error, `VR_IsHmdPresent`, one `VREvent_Quit`, which `Submit` fails, and that nothing ends or crashes.
`exiting_old_events` polls that quit through IVRSystem_009, whose game passes SDK 0.9.x's 32-byte `VREvent_t`
(case `old-events`): nothing may be written past it. The script
format is described at the top of `tests/runtime/script.h`.

A debug build of the DLL (`-DCMAKE_BUILD_TYPE=Debug -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDebug`) with
`OC_CRT_LEAK_REPORT_DIR=<dir>` set writes the debug C runtime's report of what was never freed to `<dir>/<pid>.txt`
when it unloads, so a run such as `xr_failures.py --only fail_session` shows what a failed start-up leaves behind.

## The Steam client

The Steam client connects to the VR runtime by itself, with no game running. `--cases observer` (Windows, with the stand-in runtime) records what that
does to the OpenXR runtime: run it from a copy of `oc_conformance.exe` named `steam.exe` (`expect/observer_steam.json`:
the Background connection refused, and the headset check answered with the stand-in runtime never started) and from
`oc_conformance.exe` itself (`expect/observer_game.json`: the same check starts the runtime). The stand-in writes its
dump when an instance ends, and the case reads that as the runtime having been started.

## Elevated shells

An elevated process's OpenXR loader ignores `XR_RUNTIME_JSON`. From an elevated shell `run.py` starts every child at
medium integrity (`tests/unelevated.py`). On CI, where everything is elevated, `--ci` makes the chosen runtime the
machine's active OpenXR runtime instead, in both registry views (a 32-bit loader reads the 32-bit one), and doesn't
put the old one back: use `--ci` only on a throwaway machine.

## Methods not called against SteamVR

`unsafe_methods.txt` lists methods that would start programs, write firmware or change SteamVR's own configuration;
with `--real-runtime` (SteamVR) they are recorded as skipped.
