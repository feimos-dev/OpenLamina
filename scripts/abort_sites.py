#!/usr/bin/env python3
"""The places where OpenLamina can end a program (hard stops), and the check that no new one comes in.

  abort_sites.py --list               write tests/abort_sites.csv: file, line, function, macro, message, return_type
  abort_sites.py --write-allowlist    write scripts/fatal_sites.txt from what's there now (reason "to convert")
  abort_sites.py --check              exit 1 if a hard stop isn't covered by scripts/fatal_sites.txt
  abort_sites.py --self-test

fatal_sites.txt: one line per function and macro, "file|function|macro|count|reason". A function may have up to count
uses of that macro; a new one, or one in a function not listed, fails the check.
"""

import argparse
import re
import sys
import tempfile
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DIRS = ["OpenOVR", "OCOVR", "DrvOpenXR"]
MACROS = ["STUBBED", "OOVR_ABORT_T", "OOVR_ABORTF", "OOVR_ABORT", "OOVR_FAILED_XR_ABORT", "OOVR_FALSE_ABORT",
          "OOVR_FAILED_DX_ABORT", "OOVR_FAILED_VK_ABORT", "ERR", "LINUX_STUBBED", "XR_STUBBED", "OC_FATAL",
          # Config.cpp's ABORT (exit()), and DX11Compositor::ThrowIfFailed and `throw` (an uncaught exception ends it)
          "ABORT", "ThrowIfFailed",
          # What every abort macro calls, when called itself
          "oovr_abort_raw"]
# The C++ library's and Windows' own, also as std:: or :: (a message box stalls a game whose window is in the headset)
CALLS = ["exit", "_Exit", "_exit", "quick_exit", "abort", "terminate", "ExitProcess", "TerminateProcess", "FatalExit",
         "FatalAppExitA", "FatalAppExitW", "__fastfail", "MessageBoxA", "MessageBoxW"]
FUNC = re.compile(r"^(?!\s)(?!(?:if|for|while|switch|return|else|do|case|#|//|typedef|namespace|class|struct|enum)\b)"
                  r".*?\b((?:\w+::)*~?\w+)\s*\([^;]*$")
DEFINE = re.compile(r"^\s*#\s*define\s+(\w+)")


def use_pattern(macros):
    return re.compile(r"(?<![\w#.:>])(" + "|".join(sorted(macros, key=len, reverse=True)) + r")\s*\(|"
                      r"(?<![\w#.:>])(?:std::|::)?(" + "|".join(CALLS) + r")\s*\(|(?<![\w#])(throw)\b")


def sources(root):
    for d in DIRS:
        base = root / d
        if base.exists():
            yield from sorted(list(base.rglob("*.cpp")) + list(base.rglob("*.h")))


def defines(root):
    """{name: body} of every #define, its continued lines joined (the first definition of a name)."""
    found = {}
    for path in sources(root):
        name, body = None, []
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            if name is None:
                m = DEFINE.match(line)
                if not m:
                    continue
                name, body = m.group(1), [line[m.end():]]
            else:
                body.append(line)
            if not line.rstrip().endswith("\\"):
                found.setdefault(name, " ".join(b.split("//", 1)[0] for b in body))
                name = None
    return found


def hard_stop_macros(root):
    """MACROS and every macro whose body reaches a hard stop, also through another such macro."""
    known = set(MACROS)
    defs = defines(root)
    for _ in range(len(defs) + 1):
        pattern = use_pattern(known)
        new = {name for name, body in defs.items() if name not in known and pattern.search(body)}
        if not new:
            break
        known |= new
    return known


def scan(root):
    sites = []
    use_re = use_pattern(hard_stop_macros(root))
    for path in sources(root):
        rel = path.relative_to(root).as_posix()
        func, ret = "<file>", ""
        in_block = False
        in_define = False
        for n, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            stripped = line.strip()
            # A macro's definition and its continued lines are not uses (hard_stop_macros reads them)
            if in_define or DEFINE.match(line):
                in_define = line.rstrip().endswith("\\")
                continue
            if in_block:
                if "*/" in stripped:
                    in_block = False
                continue
            if stripped.startswith("/*") and "*/" not in stripped:
                in_block = True
                continue
            if stripped.startswith("//") or stripped.startswith("*"):
                continue
            m = FUNC.match(line)
            if m:
                func = m.group(1)
                ret = " ".join(line[:m.start(1)].split())
            code = line.split("//", 1)[0]
            for use in use_re.finditer(code):
                macro = use.group(1) or use.group(2) or use.group(3)
                if macro == "ThrowIfFailed" and "::ThrowIfFailed" in code:
                    continue  # its definition, not a use
                if macro == "oovr_abort_raw" and re.match(r"\s*(?:OC_NORETURN\s+)?void\s", code):
                    continue  # its definition
                message = code[use.end():].strip().rstrip(";").rstrip(")")[:100]
                sites.append((rel, n, func, macro, message, ret))
    return sites


def read_allowlist(path):
    allowed = {}
    if path.exists():
        for line in path.read_text(encoding="utf-8").splitlines():
            if not line.strip() or line.startswith("#"):
                continue
            parts = line.split("|")
            if len(parts) >= 4:
                allowed[(parts[0], parts[1], parts[2])] = int(parts[3])
    return allowed


def counts(sites):
    return Counter((s[0], s[2], s[3]) for s in sites)


def check(root, allowlist):
    allowed = read_allowlist(allowlist)
    bad = []
    for key, n in sorted(counts(scan(root)).items()):
        if n > allowed.get(key, 0):
            bad.append(f"{key[0]}: {n} x {key[2]} in {key[1]} (allowed {allowed.get(key, 0)})")
    for b in bad:
        print("not allowed:", b)
    print(f"{sum(counts(scan(root)).values())} hard stops, {len(bad)} not allowed")
    return 1 if bad else 0


def write_allowlist(root, path):
    lines = ["# Hard stops that may stay, checked by scripts/abort_sites.py --check: file|function|macro|count|reason"]
    for (f, func, macro), n in sorted(counts(scan(root)).items()):
        lines.append(f"{f}|{func}|{macro}|{n}|to convert")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def self_test():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "OpenOVR").mkdir()
        (root / "OpenOVR" / "a.cpp").write_text(
            "int Foo::Bar(int x)\n{\n\tSTUBBED();\n}\n// OOVR_ABORT(\"comment\");\nvoid Baz()\n{\n"
            "\tOOVR_ABORTF(\"x %d\", 1);\n\tOOVR_SOFT_ABORT(\"soft\");\n\tstd::terminate();\n\tapp->exit(0);\n"
            "\toovr_abort_raw(__FILE__, 1, \"f\", \"m\");\n\t_exit(1);\n}\n"
            "OC_NORETURN void oovr_abort_raw(const char* file, long line)\n{\n}\n"
            # A macro reaching a hard stop, through another one, over continued lines: its uses count, its body doesn't
            "#define WRAP(x) \\\n\tdo { \\\n\t\tOOVR_ABORT(x); \\\n\t} while (0)\n#define WRAP2(x) WRAP(x)\n"
            "#define QUIET(x) OOVR_LOG(x)\nvoid Qux()\n{\n\tWRAP2(\"a\");\n\tQUIET(\"b\");\n}\n")
        sites = scan(root)
        got = [(s[2], s[3]) for s in sites]
        want = [("Foo::Bar", "STUBBED"), ("Baz", "OOVR_ABORTF"), ("Baz", "terminate"), ("Baz", "oovr_abort_raw"),
                ("Baz", "_exit"), ("Qux", "WRAP2")]
        if got != want:
            print("self-test FAILED: got", got, "want", want)
            return 1
        allow = root / "fatal.txt"
        write_allowlist(root, allow)
        if check(root, allow) != 0:
            print("self-test FAILED: a fresh allowlist doesn't pass")
            return 1
        (root / "OpenOVR" / "a.cpp").write_text((root / "OpenOVR" / "a.cpp").read_text() + "void New()\n{\n\tERR(\"x\");\n}\n")
        if check(root, allow) != 1:
            print("self-test FAILED: a new hard stop passes")
            return 1
    print("self-test passed")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--write-allowlist", action="store_true")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    allowlist = ROOT / "scripts" / "fatal_sites.txt"
    if args.self_test:
        return self_test()
    if args.list:
        out = ROOT / "tests" / "abort_sites.csv"
        rows = ["file,line,function,macro,message,return_type"]
        for f, n, func, macro, msg, ret in scan(ROOT):
            rows.append(",".join([f, str(n), func, macro, '"' + msg.replace('"', "'") + '"', '"' + ret + '"']))
        out.write_text("\n".join(rows) + "\n", encoding="utf-8", newline="\n")
        print(f"wrote {out}: {len(rows) - 1} hard stops")
    if args.write_allowlist:
        write_allowlist(ROOT, allowlist)
        print(f"wrote {allowlist}")
    if args.check:
        return check(ROOT, allowlist)
    return 0


if __name__ == "__main__":
    sys.exit(main())
