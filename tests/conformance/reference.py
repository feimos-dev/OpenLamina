#!/usr/bin/env python3
"""Writes a recording's results.json as a reference to compare with (expected/steamvr-<version>.json): still JSON, with
one record per line, sorted by interface, app type, argument set and index, so a new recording's diff is one line for
each answer that changed.

  reference.py <results.json> <expected.json>
"""

import json
import sys
from pathlib import Path


def sort_key(r):
    index = r.get("index")
    return (r.get("case") or r.get("interface", ""), str(r.get("apptype", "")), r.get("argset") or "",
            index if isinstance(index, int) else -1, str(r.get("method", "")), r.get("round") or 0)


def dumps(results):
    head = {k: v for k, v in results.items() if k != "records"}
    lines = [json.dumps(r, sort_keys=True, ensure_ascii=False, separators=(",", ":"))
             for r in sorted(results.get("records", []), key=sort_key)]
    text = json.dumps(head, sort_keys=True, ensure_ascii=False)[:-1]
    text += (", " if head else "") + '"records": [\n' + ",\n".join(lines) + "\n]}\n"
    return text


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    results = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
    text = dumps(results)
    if json.loads(text) != {**results, "records": sorted(results.get("records", []), key=sort_key)}:
        sys.exit("reference.py: the written reference doesn't read back as the results")
    Path(sys.argv[2]).write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {sys.argv[2]}: {len(results.get('records', []))} records")


if __name__ == "__main__":
    main()
