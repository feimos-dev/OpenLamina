#!/usr/bin/env python3
"""Runs OpenLamina against the stand-in OpenXR runtime with each failure script (scripts/<name>.json) and checks
what expect/xr_failures.json says it should do: start-up errors, dropped frames, session loss, failed swapchains
and binding suggestions. Nothing may end or crash in any of them.

  xr_failures.py --runtime build-tests/bin/vrclient_x64.dll --conformance build-tests/tests/oc_conformance.exe --out <dir>
                 [--only <script>] [--ci]
"""

import argparse
import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
VREVENT_QUIT = 700


def records_of(out):
    return json.loads((out / "results.json").read_text(encoding="utf-8"))["records"]


def check(name, want, out):
    """Returns the list of what didn't match for one script's run in out."""
    misses = []
    records = records_of(out)
    for r in records:
        if r.get("status") != "ok":
            misses.append(f"{r.get('case')}/{r.get('method')}: {r.get('status')} (exit {r.get('exit_code')})")
    by_method = {(r.get("case"), r.get("method")): r for r in records}

    for method, expected in want.get("init_error", {}).items():
        r = by_method.get(("init-matrix", method))
        got = r.get("init_error") if r else None
        if got != expected:
            misses.append(f"init-matrix/{method} init_error: got {got}, expected {expected}")
    if "hmd_present" in want:
        r = by_method.get(("init-matrix", "BIsHmdPresent"))
        got = r.get("ret") if r else None
        if got != want["hmd_present"]:
            misses.append(f"BIsHmdPresent: got {got}, expected {want['hmd_present']}")

    frames = [r for r in records if r.get("case") == "frame-loop" and str(r.get("method", "")).startswith("frame ")]
    if "frames" in want and len(frames) != want["frames"]:
        misses.append(f"frame-loop: {len(frames)} frames recorded, expected {want['frames']}")
    # Every frame-loop run: no swapchain image left acquired, and the call order never broken
    dump_path = out / "raw" / "case-frame-loop.dump.json"
    if "frame-loop" in want.get("cases", []) and not dump_path.exists():
        misses.append("frame-loop: the runtime wrote no dump (its instance never destroyed?)")
    if dump_path.exists():
        dump = json.loads(dump_path.read_text(encoding="utf-8"))
        if dump.get("images_left_acquired", 0):
            misses.append(f"frame-loop: {dump['images_left_acquired']} swapchain images still acquired when their swapchain went")
        if dump.get("swapchain_call_order_errors"):
            misses.append(f"frame-loop: swapchain calls out of order: {dump['swapchain_call_order_errors']}")
    if "frames_ended_min" in want:
        dump_path = out / "raw" / "case-frame-loop.dump.json"
        dump = json.loads(dump_path.read_text(encoding="utf-8")) if dump_path.exists() else {}
        ended = dump.get("frames_ended", -1)
        if ended < want["frames_ended_min"]:
            misses.append(f"frame-loop: the runtime saw {ended} frames ended, expected at least {want['frames_ended_min']}")
    if "quit_events" in want:
        quits = [int(r["method"].split()[1]) for r in frames for e in r.get("events", []) if e == VREVENT_QUIT]
        if len(quits) != want["quit_events"]:
            misses.append(f"frame-loop: {len(quits)} VREvent_Quit polled (frames {quits}), expected {want['quit_events']}")
        elif "quit_from_frame" in want and quits and quits[0] < want["quit_from_frame"]:
            misses.append(f"frame-loop: VREvent_Quit polled at frame {quits[0]}, expected from frame {want['quit_from_frame']}")
    if "device_events" in want:
        # A loss: the devices deactivated (101), then activated again (100) on the new session, in that order (after
        # any activations at the start); the runtime asks for no quit (704)
        seq = [e for r in frames for e in r.get("events", []) if e in (100, 101, 704)]
        rest = iter(seq)
        if not all(any(e == w for e in rest) for w in want["device_events"]):
            misses.append(f"frame-loop: device events {seq}, expected {want['device_events']} in that order")
        if 704 in seq:
            misses.append("frame-loop: VREvent_DriverRequestedQuit polled after a loss")
    for key, at_least in (("sessions_lost", False), ("sessions_created_min", True), ("instances_lost", False), ("instances_created_min", True)):
        if key in want:
            dump = json.loads(dump_path.read_text(encoding="utf-8")) if dump_path.exists() else {}
            name = key.removesuffix("_min")
            got = dump.get(name, 0)
            if (got < want[key]) if at_least else (got != want[key]):
                misses.append(f"frame-loop: the runtime saw {got} {name.replace('_', ' ')}, expected {'at least ' if at_least else ''}{want[key]}")
    if "loss_record" in want:
        # A helper program's loss (case overlay-app-loss): its device events in order, no quit, and a session made again
        # that ran frames
        r = by_method.get(("overlay-app-loss", "loss")) or {}
        want_r = want["loss_record"]
        events = r.get("events", [])
        rest = iter(events)
        if not all(any(e == w for e in rest) for w in want_r["device_events"]):
            misses.append(f"overlay-app-loss: device events {events}, expected {want_r['device_events']} in that order")
        if 700 in events or 704 in events:
            misses.append(f"overlay-app-loss: a quit polled after a loss ({events})")
        for key, value in want_r.items():
            if key.endswith("_min") and (r.get(key.removesuffix("_min")) or 0) < value:
                misses.append(f"overlay-app-loss: {key.removesuffix('_min')} {r.get(key.removesuffix('_min'))}, expected at least {value}")
            elif key in ("sessions_lost", "instances_lost", "manifest") and r.get(key) != value:
                misses.append(f"overlay-app-loss: {key} {r.get(key)}, expected {value}")
    if "rebuilt_actions" in want:
        # Case input-frames through an instance loss: the last instance has the game's manifest's actions and their
        # bindings
        path = out / "raw" / "case-input-frames.dump.json"
        dump = json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}
        if dump.get("instances_lost") != 1 or dump.get("instances_created", 0) < 2:
            misses.append(f"input-frames: {dump.get('instances_lost')} instances lost and {dump.get('instances_created')} made, expected 1 and at least 2")
        names = {a.get("name") for a in dump.get("actions", [])}
        missing = [n for n in want["rebuilt_actions"] if n not in names]
        if missing:
            misses.append(f"input-frames: the new instance has no actions {missing}")
        if not dump.get("suggested_bindings"):
            misses.append("input-frames: the new instance had no bindings suggested")
    if "action_attaches_min" in want:
        # Case input-frames through a session or instance loss: the game's actions are attached to the session made
        # again too
        path = out / "raw" / "case-input-frames.dump.json"
        dump = json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}
        lost = (dump.get("sessions_lost") or 0) + (dump.get("instances_lost") or 0)
        if lost != 1 or dump.get("action_attaches", 0) < want["action_attaches_min"]:
            misses.append(f"input-frames: {lost} sessions or instances lost and {dump.get('action_attaches')} action attaches, expected 1 and at least {want['action_attaches_min']}")
    if "old_event_quits" in want:
        # IVRSystem_009's events (case old-events): the quit arrives, and nothing is written past the 32-byte event
        olds = [r for r in records if r.get("case") == "old-events" and str(r.get("method", "")).startswith("frame ")]
        quits = sum(1 for r in olds for e in r.get("events", []) if e == VREVENT_QUIT)
        overrun = sum(r.get("overrun", 0) for r in olds)
        if len(olds) != 120:
            misses.append(f"old-events: {len(olds)} frames recorded, expected 120")
        if quits != want["old_event_quits"]:
            misses.append(f"old-events: {quits} VREvent_Quit polled, expected {want['old_event_quits']}")
        if overrun:
            misses.append(f"old-events: {overrun} bytes written past the 32-byte event")
    if "failed_submits" in want:
        # The Submit that first makes the swapchain gets the failure (frame 1's left eye restarts the session for
        # the game's graphics API, so that's the right eye); every other Submit works
        answers = [r.get(eye) for r in frames for eye in ("left", "right")]
        failed = want["failed_submits"]
        got = answers.count(failed["answer"])
        if got != failed["count"]:
            misses.append(f"frame-loop: {got} Submits answered {failed['answer']}, expected {failed['count']}")
        others = [a for a in answers if a not in (failed["answer"], want.get("other_submits", 0))]
        if others:
            misses.append(f"frame-loop: Submits also answered {sorted(set(others))}")
    if want.get("manifest_handles"):
        for method in ("GetActionSetHandle", "GetActionHandle trigger", "GetActionHandle pose"):
            r = by_method.get(("input-manifest", method))
            if not r or r.get("ret") != 0:
                misses.append(f"input-manifest/{method}: got {r.get('ret') if r else 'no record'}, expected 0")
    return misses


def self_test():
    """check: a frame-loop run whose runtime wrote no dump fails"""
    import tempfile
    with tempfile.TemporaryDirectory() as folder:
        out = Path(folder)
        (out / "raw").mkdir()
        (out / "results.json").write_text(json.dumps({"records": []}), encoding="utf-8")
        misses = check("self-test", {"cases": ["frame-loop"]}, out)
    if len(misses) != 1:
        print(f"self-test FAILED: {misses}")
        return 1
    print("self-test passed")
    return 0


def main():
    if sys.argv[1:] == ["--self-test"]:
        return self_test()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--runtime", required=True)
    ap.add_argument("--conformance", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--only")
    ap.add_argument("--ci", action="store_true", help="passed to run.py (CI runners are elevated)")
    args = ap.parse_args()
    expect = json.loads((HERE / "expect" / "xr_failures.json").read_text(encoding="utf-8"))
    failed = 0
    for name, want in expect.items():
        if name.startswith("#") or (args.only and name != args.only):
            continue
        out = Path(args.out) / name
        cmd = [sys.executable, "-I", str(HERE / "run.py"), "--runtime", args.runtime, "--conformance", args.conformance,
               "--xr-runtime", "stand-in", "--script", str(HERE / "scripts" / f"{name}.json"),
               "--cases", ",".join(want["cases"]), "--allow-failures", "--jobs", "2", "--out", str(out),
               "--manifest", str(HERE.parent / "unit" / "data" / "manifest_basic.json")] + (["--ci"] if args.ci else [])
        # Nothing of an earlier run into the same --out is read as this one's
        (out / "results.json").unlink(missing_ok=True)
        for dump in (out / "raw").glob("*.dump.json") if (out / "raw").exists() else []:
            dump.unlink()
        run = subprocess.run(cmd, capture_output=True, text=True)
        if run.returncode != 0 or not (out / "results.json").exists():
            print(f"{name}: FAILED: run.py exited {run.returncode}")
            for line in (run.stdout + run.stderr).splitlines()[-20:]:
                print(f"  {line}")
            failed += 1
            continue
        misses = check(name, want, out)
        print(f"{name}: {'ok' if not misses else 'FAILED'}")
        for m in misses:
            print(f"  {m}")
        failed += bool(misses)
    print(f"{failed} of the scripts didn't do what expect/xr_failures.json says")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
