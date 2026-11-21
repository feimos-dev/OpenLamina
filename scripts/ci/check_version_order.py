#!/usr/bin/env python3
"""Checks install.sh's release order against tests/version_order.json, the table the app's updater is tested with.

  scripts/ci/check_version_order.py [installer/install.sh] [tests/version_order.json]

Each row [a, b, sign]: install.sh's "newest" offers a release a to an install of b only when a is newer (sign 1), and
b to an install of a only when b is newer (sign -1). The Python is taken from install.sh itself and given a release
list on standard input; nothing is downloaded or installed.
"""

import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
ASSET = "OpenLamina-Linux.tar.gz"


def embedded_python(install_sh):
    text = install_sh.read_text(encoding="utf-8").replace("\r\n", "\n")
    start = text.find("PY=$(\n\tcat <<'EOF'\n")
    if start < 0:
        sys.exit(f"check_version_order.py: no embedded Python (PY=$(cat <<'EOF' ...) in {install_sh}")
    body = text[start:].split("\n", 2)[2]
    end = body.find("\nEOF\n")
    if end < 0:
        sys.exit(f"check_version_order.py: the embedded Python in {install_sh} has no end")
    return body[:end + 1]


def offered(py, release, installed):
    """True if "newest" offers release (a version, without "v") to an install of installed."""
    releases = [{
        "tag_name": "v" + release, "prerelease": True, "draft": False,
        "assets": [{"name": ASSET, "digest": "sha256:" + "0" * 64,
                    "browser_download_url": f"https://github.com/o/r/releases/download/v{release}/{ASSET}"}],
    }]
    r = subprocess.run([sys.executable, "-I", "-c", py, "newest", installed, "1", ASSET], input=json.dumps(releases),
                       capture_output=True, text=True, timeout=30)
    if r.returncode != 0:
        sys.exit(f"check_version_order.py: newest {release!r} over {installed!r} failed: {r.stderr.strip()}")
    return r.stdout.split("\t")[0].strip() == release


def main():
    install_sh = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "installer" / "install.sh"
    table = Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / "tests" / "version_order.json"
    py = embedded_python(install_sh)
    rows = json.loads(table.read_text(encoding="utf-8"))["rows"]
    if not rows:
        sys.exit(f"check_version_order.py: {table} has no rows, so nothing would be checked")
    bad = []
    for a, b, sign in rows:
        if offered(py, a, b) != (sign > 0):
            bad.append(f"{a} over an install of {b}: {'not ' if sign > 0 else ''}offered, but the sign is {sign}")
        if offered(py, b, a) != (sign < 0):
            bad.append(f"{b} over an install of {a}: {'not ' if sign < 0 else ''}offered, but the sign is {sign}")
    for line in bad:
        print("install.sh orders versions differently:", line)
    print(f"{len(rows)} rows of {table.name}: {len(bad)} differences")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
