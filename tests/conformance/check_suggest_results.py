#!/usr/bin/env python3
"""Checks that the runtime took every set of bindings OpenLamina suggested: reads the "Suggested <profile>: <result>
(<n> bindings)" lines of OpenLamina's log and fails on any answer other than XR_SUCCESS. A runtime takes a profile's
bindings all or none, so one refused path loses the whole controller.

  check_suggest_results.py <openlamina.log> [--runtime-log <the program's own output>]

With --runtime-log, the first line of the program's own output (its stderr, where Monado's client part logs) that names
each refused profile is printed too: Monado names the path it refused there."""

import argparse
import re
import sys
from pathlib import Path

LINE = re.compile(r"Suggested (\S+): (\S+) \((\d+) bindings\)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--runtime-log", help="the program's own output (Monado's client log, its stderr)")
    args = ap.parse_args()

    answers = {}
    for line in Path(args.log).read_text(encoding="utf-8", errors="replace").splitlines():
        m = LINE.search(line)
        if m:
            answers.setdefault(m.group(1), []).append((m.group(2), int(m.group(3))))
    if not answers:
        print("no suggested bindings in the log")
        return 1

    refused = sorted(p for p, results in answers.items() if any(r != "XR_SUCCESS" for r, _ in results))
    for profile, results in sorted(answers.items()):
        print(f"{profile}: " + ", ".join(f"{r} ({n})" for r, n in results))
    if refused and args.runtime_log:
        output = Path(args.runtime_log).read_text(encoding="utf-8", errors="replace").splitlines()
        for profile in refused:
            first = next((line.strip() for line in output if profile in line), "(not named)")
            print(f"\n{profile} in the runtime's log: {first}")
    print(f"\n{len(answers)} profiles, {len(refused)} refused")
    return 1 if refused else 0


if __name__ == "__main__":
    sys.exit(main())
