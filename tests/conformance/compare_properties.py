#!/usr/bin/env python3
"""Compares two property-sweep records (the property-sweep case): SteamVR's, recorded with real devices, and
OpenLamina's on the same hardware. Per device (matched by name: "HMD", "Controller left", "TrackingReference 1"),
lists the properties SteamVR answers and OpenLamina doesn't, those both answer differently, those OpenLamina
answers and SteamVR doesn't, and those neither answers but with different errors.

  compare_properties.py <steamvr results.json or .jsonl> <openlamina results.json or .jsonl> [--markdown]

A value is a property's answer; "<name> error" is the error code of one not answered (4: unknown property). Exit code 0;
it reports, it doesn't judge (some differences are the hardware's: a serial, a battery level).
"""

import json
import sys
from pathlib import Path

ERRORS = {0: "Success", 1: "WrongDataType", 2: "WrongDeviceClass", 3: "BufferTooSmall", 4: "UnknownProperty",
          5: "InvalidDevice", 6: "CouldNotContactServer", 7: "ValueNotProvidedByDevice", 8: "StringExceedsMaximumLength",
          9: "NotYetAvailable", 10: "PermissionDenied", 11: "InvalidOperation", 12: "CannotWriteToWildcards",
          13: "IPCReadFailure", 14: "OutOfMemory", 15: "InvalidContainer"}
SKIP = {"case", "method", "index", "status", "round", "apptype", "argset", "interface", "crash", "skipped", "resumed",
        "exit_code"}


def devices(path):
    text = Path(path).read_text(encoding="utf-8")
    try:
        records = json.loads(text)["records"]  # run.py's results.json
    except (json.JSONDecodeError, KeyError, TypeError):
        records = [json.loads(line) for line in text.splitlines() if line.strip()]  # a raw .jsonl
    out = {}
    for r in records:
        if r.get("case") == "property-sweep" and str(r.get("method", "")).startswith("device "):
            name = r["method"][len("device "):]
            if name in out:
                print(f"{path}: two records of device {name}: the later one is compared", file=sys.stderr)
            out[name] = r
    return out


def answers(record):
    """name -> (answered, value or error code)"""
    out = {}
    for k, v in record.items():
        if k in SKIP:
            continue
        if k.endswith(" error"):
            out[k[:-len(" error")]] = (False, v)
        else:
            out[k] = (True, v)
    return out


def show(value):
    text = json.dumps(value, ensure_ascii=False)
    return text if len(text) <= 80 else text[:77] + "..."


def cell(value, error):
    """A value or an error code as printed"""
    return ERRORS.get(value, value) if error else show(value)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 2:
        sys.exit(__doc__)
    md = "--markdown" in sys.argv
    steamvr, openlamina = devices(args[0]), devices(args[1])
    for name in sorted(set(steamvr) | set(openlamina)):
        print(f"\n## {name}\n" if md else f"\n=== {name}")
        if name not in openlamina:
            print(f"Only SteamVR has this device ({len(answers(steamvr[name]))} properties read).")
            continue
        if name not in steamvr:
            print("Only OpenLamina has this device.")
            continue
        s, o = answers(steamvr[name]), answers(openlamina[name])
        missing = [(k, s[k][1], o.get(k, (False, None))[1]) for k in sorted(s) if s[k][0] and not o.get(k, (False,))[0]]
        differ = [(k, s[k][1], o[k][1]) for k in sorted(s) if s[k][0] and o.get(k, (False,))[0] and s[k][1] != o[k][1]]
        extra = [(k, o[k][1], s.get(k, (False, None))[1]) for k in sorted(o) if o[k][0] and not s.get(k, (False,))[0]]
        errors = [(k, s[k][1], o[k][1]) for k in sorted(s) if not s[k][0] and k in o and not o[k][0] and s[k][1] != o[k][1]]
        same = sum(1 for k in s if s[k][0] and o.get(k, (False,))[0] and s[k][1] == o[k][1])
        print(f"{same} the same; {len(missing)} SteamVR answers and OpenLamina doesn't; {len(differ)} answered "
              f"differently; {len(extra)} OpenLamina answers and SteamVR doesn't; {len(errors)} unanswered with "
              f"different errors.")
        for title, rows, left, right in (("SteamVR answers, OpenLamina doesn't", missing, "SteamVR", "OpenLamina error"),
                                         ("Answered differently", differ, "SteamVR", "OpenLamina"),
                                         ("OpenLamina answers, SteamVR doesn't", extra, "OpenLamina", "SteamVR error"),
                                         ("Unanswered, different errors", errors, "SteamVR error", "OpenLamina error")):
            if not rows:
                continue
            if md:
                print(f"\n### {title}\n\n| Property | {left} | {right} |\n|---|---|---|")
            else:
                print(f"-- {title}")
            for k, a, b in rows:
                a, b = cell(a, "error" in left), cell(b, "error" in right)
                if md:
                    a, b = a.replace("|", "\\|"), b.replace("|", "\\|")
                    print(f"| `{k}` | {a} | {b} |")
                else:
                    print(f"   {k}: {a}  |  {b}")


if __name__ == "__main__":
    main()
