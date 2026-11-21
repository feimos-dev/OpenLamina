#!/usr/bin/env python3
"""Inventory of the OpenVR interface versions Valve has shipped, against the ones OpenLamina offers.

  openvr_inventory.py --valve-repo <clone of github.com/ValveSoftware/openvr> --write tests/conformance/interfaces.txt
  openvr_inventory.py --valve-repo <clone> --compare tests/conformance/interfaces.txt  # fails on anything new
  openvr_inventory.py --check        # every listed version is offered (a GEN_INTERFACE line) or in not_implemented.txt
  openvr_inventory.py --write-how-real tests/conformance/how_real.txt
  openvr_inventory.py --self-test

--write-how-real writes, per interface (its OpenOVR/Reimpl/Base*.cpp), how many of the methods defined there answer
OC_UNSUPPORTED always or in some cases, or only log and return (a logged no-op), against all of them; and the
interfaces of OpenVRHeaders/custom_interfaces with why each is there (custom_interfaces.txt's reasons). --check also
checks that file is what --write-how-real would write.

A blobless clone is enough: git clone --filter=blob:none --no-checkout https://github.com/ValveSoftware/openvr.git
Every tag's headers/openvr.h gives the app-facing versions ("app"); a version found only in headers/openvr_capi.h is
reachable only through the C API ("capi"); one found only in headers/openvr_driver.h is a driver interface and left out.
"""

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

VERSION_RE = re.compile(r'"(IVR[A-Za-z]+_[0-9]{3})"')
GEN_RE = re.compile(r'^\s*GEN_INTERFACE\("(\w+)",\s*"(\d{3})"', re.M)
ROOT = Path(__file__).resolve().parent.parent


def git(repo, *args, check=False):
    r = subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True, encoding="utf-8",
                       errors="replace")
    if check and r.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)} failed in {repo}: {r.stderr.strip()}")
    return r


def show(repo, tag, path):
    """The file at the tag; "" if the tag has no such file. A file the tag has but git can't give (a blob a blobless
    clone fails to fetch) is an error, not an empty file."""
    r = git(repo, "show", f"{tag}:{path}")
    if r.returncode == 0:
        return r.stdout
    if git(repo, "ls-tree", "--name-only", tag, "--", path, check=True).stdout.strip():
        raise RuntimeError(f"git show {tag}:{path} failed in {repo}: {r.stderr.strip()}")
    return ""


def tags_in_order(repo):
    tags = [t for t in git(repo, "tag", check=True).stdout.split() if t]
    # Order by the date of the tagged commit
    dated = []
    for t in tags:
        ct = git(repo, "log", "-1", "--format=%ct", t, check=True).stdout.strip()
        dated.append((int(ct or 0), t))
    return [t for _, t in sorted(dated)]


def inventory(repo):
    """Returns ({version: (kind, first tag, last tag)}, head commit, head date)."""
    found = {}  # version -> [kinds set, first, last]
    for tag in tags_in_order(repo):
        app = set(VERSION_RE.findall(show(repo, tag, "headers/openvr.h")))
        capi = set(VERSION_RE.findall(show(repo, tag, "headers/openvr_capi.h"))) - app
        driver = set(VERSION_RE.findall(show(repo, tag, "headers/openvr_driver.h")))
        for kind, versions in (("app", app), ("capi", capi - driver)):
            for v in versions:
                entry = found.setdefault(v, [set(), tag, tag])
                entry[0].add(kind)
                entry[2] = tag
    result = {}
    for v, (kinds, first, last) in found.items():
        result[v] = ("app" if "app" in kinds else "capi", first, last)
    head = git(repo, "log", "-1", "--format=%H %cs", "HEAD").stdout.split()
    return result, (head[0] if head else "?"), (head[1] if len(head) > 1 else "?")


def version_key(v):
    name, num = v.rsplit("_", 1)
    return name, int(num)


def format_inventory(inv):
    return [f"{v} {kind} {first} {last}" for v, (kind, first, last) in sorted(inv.items(), key=lambda i: version_key(i[0]))]


def write(repo, out):
    inv, commit, date = inventory(repo)
    lines = [f"# OpenVR interface versions Valve has shipped, read from github.com/ValveSoftware/openvr at {commit} ({date})",
             "# by scripts/openvr_inventory.py. Columns: version, app (headers/openvr.h) or capi (C API only), first tag,",
             "# last tag."]
    lines += format_inventory(inv)
    Path(out).write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    return inv


def compare(repo, path):
    """What Valve's tags have that the listed file doesn't: a version it lacks, or one whose kind or tags changed (a
    newer tag still shipping it). 1 if anything is new, and 1 if a listed version is in none of Valve's tags (none
    found at all, as from a clone without tags, compares nothing)."""
    inv, commit, date = inventory(repo)
    listed = {}
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        parts = line.split()
        if parts and not line.startswith("#"):
            listed[parts[0]] = parts[1:]
    new = 0
    for v, (kind, first, last) in sorted(inv.items(), key=lambda i: version_key(i[0])):
        if v not in listed:
            print(f"new version: {v} {kind} {first} {last}")
            new += 1
        elif listed[v] != [kind, first, last]:
            print(f"changed: {v} {' '.join(listed[v])} -> {kind} {first} {last}")
            new += 1
    gone = sorted(set(listed) - set(inv), key=version_key)
    for v in gone:
        print(f"listed but not in Valve's tags: {v}")
    print(f"{len(inv)} versions in Valve's tags at {commit} ({date}), {len(listed)} listed; {new} new or changed, "
          f"{len(gone)} not found")
    if new:
        print(f"Update the list: openvr_inventory.py --valve-repo <clone> --write {path}, then offer each new version "
              "or set it aside in not_implemented.txt")
    if gone:
        print("A listed version missing from Valve's tags: the clone is incomplete, or Valve removed a tag")
    return 1 if new or gone else 0


def read_listed(path):
    listed = {}
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        listed[parts[0]] = parts[1] if len(parts) > 1 else ""
    return listed


def offered(root):
    versions = set()
    for cvr in (root / "OpenOVR" / "Reimpl").glob("CVR*.cpp"):
        for name, num in GEN_RE.findall(cvr.read_text(encoding="utf-8", errors="replace")):
            versions.add(f"IVR{name}_{num}")
    return versions


def check(root):
    listed = read_listed(root / "tests" / "conformance" / "interfaces.txt")
    skipped = read_listed(root / "tests" / "conformance" / "not_implemented.txt")
    have = offered(root)
    missing = [v for v in sorted(listed, key=version_key) if v not in have and v not in skipped]
    for v in missing:
        print(f"not offered: {v} ({listed[v]})")
    print(f"{len(listed)} listed, {len(listed) - len(missing)} offered or set aside, {len(missing)} missing")
    stale = False
    how_real_txt = root / "tests" / "conformance" / "how_real.txt"
    if not how_real_txt.exists() or how_real_txt.read_text(encoding="utf-8").replace("\r\n", "\n") != format_how_real(root):
        print(f"{how_real_txt.relative_to(root)} is out of date: scripts/openvr_inventory.py --write-how-real {how_real_txt.relative_to(root).as_posix()}")
        stale = True
    return 1 if missing or stale else 0


METHOD_RE = re.compile(r'^[A-Za-z_][\w:<>,\s\*&]*?\b(Base\w+)::(~?\w+)\s*\(', re.M)
# Headers in OpenVRHeaders/custom_interfaces that conformance doesn't call (not in custom_interfaces.txt), and why
# each is there
HEADER_ONLY_REASONS = {
    "IVRClientCore_002": "IVRClientCore's old versions, which openvr_api.dll asks vrclient for: Valve never published them",
    "IVRClientCore_003": "IVRClientCore's old versions, which openvr_api.dll asks vrclient for: Valve never published them",
    "IVRCompositor_017": "used by The Lab, never published by Valve: 018 without its grid fade methods",
}


def strip_code(text):
    """C++ with comments, string and character literals blanked out (lengths kept)."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            out.append(c + " " * (min(j, n) - i - 1) + (c if j < n else ""))
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def method_bodies(text):
    """[(class, method, body)] of the methods defined at the top level of a Base*.cpp, bodies with comments and
    literals blanked."""
    code = strip_code(text)
    found = []
    for m in METHOD_RE.finditer(code):
        # The parameter list, then the body's braces
        depth, i = 0, m.end() - 1
        while i < len(code):
            if code[i] == "(":
                depth += 1
            elif code[i] == ")":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        j = i + 1
        while j < len(code) and code[j] not in "{;":
            j += 1
        if j >= len(code) or code[j] != "{":
            continue  # a declaration, not a definition
        depth, k = 0, j
        while k < len(code):
            if code[k] == "{":
                depth += 1
            elif code[k] == "}":
                depth -= 1
                if depth == 0:
                    break
            k += 1
        found.append((m.group(1), m.group(2), code[j + 1:k]))
    return found


def how_real_kind(body):
    """'unsupported' (OC_UNSUPPORTED first thing), 'partly' (in some cases), 'noop' (only logs and returns) or 'real'."""
    statements = [st.strip() for st in re.split(r";", body) if st.strip()]
    if statements and re.match(r"OC_UNSUPPORTED(_VOID)?\s*\(", statements[0]):
        return "unsupported"
    if re.search(r"\bOC_UNSUPPORTED(_VOID)?\s*\(", body):
        return "partly"
    logs = [st for st in statements if re.match(r"OOVR_LOG\w*\s*\(", st)]
    others = [st for st in statements if st not in logs and not re.match(r"return\b", st)]
    if logs and not others and "{" not in body:
        return "noop"
    return "real"


def how_real(root):
    """{class: {kind: count}} of OpenOVR/Reimpl/Base*.cpp, leaving out constructors, destructors and OpenLamina's own
    _ methods."""
    table = {}
    for cpp in sorted((root / "OpenOVR" / "Reimpl").glob("Base*.cpp")):
        for cls, method, body in method_bodies(cpp.read_text(encoding="utf-8", errors="replace")):
            if method.startswith("~") or method == cls or method.startswith("_") or not cpp.stem == cls:
                continue
            counts = table.setdefault(cls, {"methods": 0, "unsupported": 0, "partly": 0, "noop": 0})
            counts["methods"] += 1
            kind = how_real_kind(body)
            if kind != "real":
                counts[kind] += 1
    return table


def custom_interfaces(root):
    """[(version, reason)] of the headers in OpenVRHeaders/custom_interfaces."""
    reasons = {}
    for line in (root / "tests" / "conformance" / "custom_interfaces.txt").read_text(encoding="utf-8").splitlines():
        parts = line.split(None, 1)
        if parts and not line.startswith("#"):
            reasons[parts[0]] = parts[1].strip() if len(parts) > 1 else ""
    out = []
    for h in sorted((root / "OpenVRHeaders" / "custom_interfaces").glob("IVR*.h"), key=lambda p: version_key(p.stem)):
        out.append((h.stem, reasons.get(h.stem) or HEADER_ONLY_REASONS.get(h.stem, "")))
    return out


def format_how_real(root):
    lines = ["# How real each interface is: generated by scripts/openvr_inventory.py --write-how-real. Per OpenOVR/Reimpl/Base*.cpp,",
             "# the methods defined there (not constructors or OpenLamina's own _ methods); those answering OC_UNSUPPORTED",
             "# always, or in some cases; those that only log and return (logged no-ops); and the share that is neither.",
             "# interface methods unsupported partly no-op real%"]
    total = {"methods": 0, "unsupported": 0, "partly": 0, "noop": 0}
    for cls, c in sorted(how_real(root).items()):
        for k in total:
            total[k] += c[k]
        real = 100 * (c["methods"] - c["unsupported"] - c["noop"]) // max(c["methods"], 1)
        lines.append(f'{cls[4:]} {c["methods"]} {c["unsupported"]} {c["partly"]} {c["noop"]} {real}')
    real = 100 * (total["methods"] - total["unsupported"] - total["noop"]) // max(total["methods"], 1)
    lines.append(f'all {total["methods"]} {total["unsupported"]} {total["partly"]} {total["noop"]} {real}')
    lines += ["", "# Interface versions of OpenLamina's own (OpenVRHeaders/custom_interfaces), and why each is there"]
    for version, reason in custom_interfaces(root):
        lines.append(f"{version} {reason}".rstrip())
    return "\n".join(lines) + "\n"


def self_test():
    with tempfile.TemporaryDirectory() as tmp:
        repo = Path(tmp)
        git(repo, "init", "-q")
        git(repo, "config", "user.email", "test@example.invalid")
        git(repo, "config", "user.name", "test")
        headers = repo / "headers"
        headers.mkdir()

        def commit(tag, openvr, capi, driver, when):
            (headers / "openvr.h").write_text(openvr)
            (headers / "openvr_capi.h").write_text(capi)
            (headers / "openvr_driver.h").write_text(driver)
            git(repo, "add", "-A")
            env_date = f"{when} +0000"
            subprocess.run(["git", "-C", str(repo), "commit", "-q", "-m", tag],
                           env={**__import__("os").environ, "GIT_COMMITTER_DATE": env_date, "GIT_AUTHOR_DATE": env_date},
                           check=True)
            git(repo, "tag", tag)

        commit("t1", '"IVRSystem_001" "IVRFoo_001"', "", '"IVRDriverThing_001"', "2020-01-01T00:00:00")
        commit("t2", '"IVRSystem_002"', '"IVRPaths_001" "IVRSystem_002"', '"IVRServerDriverHost_005"',
               "2021-01-01T00:00:00")
        inv, _, _ = inventory(repo)
        got = format_inventory(inv)
        want = ["IVRFoo_001 app t1 t1", "IVRPaths_001 capi t2 t2", "IVRSystem_001 app t1 t1", "IVRSystem_002 app t2 t2"]
        if got != want:
            print("self-test FAILED")
            print("  got: ", got)
            print("  want:", want)
            return 1
        listed = repo / "listed.txt"
        listed.write_text("\n".join(want) + "\n")
        bad = []
        if compare(repo, listed) != 0:
            bad.append("the same versions compare as new")
        listed.write_text("\n".join(want + ["IVRGone_001 app t1 t1"]) + "\n")
        if compare(repo, listed) != 1:
            bad.append("a listed version missing from the tags passes")
        empty = repo / "empty"
        empty.mkdir()
        git(empty, "init", "-q")
        if compare(empty, listed) != 1:
            bad.append("a clone without tags passes")
        try:
            show(repo / "not-a-repo", "t1", "headers/openvr.h")
            bad.append("a failing git gives an empty file")
        except RuntimeError:
            pass
        if bad:
            print("self-test FAILED:", "; ".join(bad))
            return 1
    # The kinds of method body
    cpp = """int BaseFoo::Gone() { OC_UNSUPPORTED(0, "no"); }
void BaseFoo::Half(int a)
{
	if (a) // "{" in a comment
		OC_UNSUPPORTED_VOID("not that");
	Do(a);
}
bool BaseFoo::Quiet() { OOVR_LOG_ONCE("No implementation"); return false; }
float BaseFoo::Real() const { return 1.0f; }
BaseFoo::BaseFoo() { OC_UNSUPPORTED_VOID("a constructor isn't a method"); }
void BaseFoo::_Own() { }
"""
    got = [(m, how_real_kind(b)) for _, m, b in method_bodies(cpp)]
    want = [("Gone", "unsupported"), ("Half", "partly"), ("Quiet", "noop"), ("Real", "real"), ("_Own", "real")]
    if got != want:
        print("self-test FAILED")
        print("  got: ", got)
        print("  want:", want)
        return 1
    print("self-test passed")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--valve-repo")
    ap.add_argument("--write")
    ap.add_argument("--compare", help="the list to compare Valve's tags with (interfaces.txt): fails on anything new")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--write-how-real", metavar="FILE")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if args.write:
        if not args.valve_repo:
            ap.error("--write needs --valve-repo")
        inv = write(args.valve_repo, args.write)
        kinds = [k for k, _, _ in inv.values()]
        print(f"wrote {args.write}: {kinds.count('app')} app, {kinds.count('capi')} capi")
    if args.write_how_real:
        Path(args.write_how_real).write_text(format_how_real(ROOT), encoding="utf-8", newline="\n")
        print(f"wrote {args.write_how_real}")
    if args.compare:
        if not args.valve_repo:
            ap.error("--compare needs --valve-repo")
        return compare(args.valve_repo, args.compare)
    if args.check:
        return check(ROOT)
    if not (args.write or args.check or args.write_how_real):
        ap.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
