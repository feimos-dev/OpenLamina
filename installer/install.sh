#!/bin/sh
# OpenLamina's Linux installer, in the OpenLamina folder of OpenLamina-Linux.tar.gz and in the installed copy.
# It installs for the current user only: no root. ./install.sh --help says what it does.
set -eu

REPOSITORY=feimos-dev/OpenLamina
FLATPAK_STEAM=com.valvesoftware.Steam
ARCHIVE=OpenLamina-Linux.tar.gz

usage() {
	cat <<'EOF'
Usage: install.sh [command] [--flatpak | --native]

Commands (one; with none, install):
  (none)            Copy OpenLamina to ~/.local/share/OpenLamina and make OpenVR games use it: it goes first in
                    the "runtime" list of ~/.config/openvr/openvrpaths.vrpath, other entries kept after it. The file
                    is backed up first, to openvrpaths.vrpath.<date and time>.bak. Files you put in bin/
                    (openlamina.ini, apps-config.json) are kept.
  --no-switch       Install or update the copy, but leave openvrpaths.vrpath as it is.
  --openlamina      Make OpenVR games use the installed copy (--opencomposite, its name before, does the same).
                    Files you put in bin/ named openlamina.ini or, from before, opencomposite.ini both work.
  --steamvr         Make OpenVR games use SteamVR again: the copy is taken out of the list and SteamVR's folder put
                    first. Starting SteamVR from Steam does that too.
  --uninstall       --steamvr, then delete the installed copy. Files you put in its bin/ are moved aside first, to
                    OpenLamina-kept-<date and time> next to it.
  --status          Say what OpenVR games use, what is installed, and the OpenXR runtime.
  --update [--pre]  Download the newest release from github.com/feimos-dev/OpenLamina (--pre: pre-releases too),
                    check its SHA-256, and install it if it is newer. The switch is left as it is.
  --launch-options  Print the Steam launch options games in the Steam Runtime (most Proton games) need to reach your
                    OpenXR runtime.
  -h, --help        This.

Steam installed as a Flatpak (com.valvesoftware.Steam) has its own openvrpaths.vrpath and can't see
~/.local/share, so for it the copy goes in ~/.var/app/com.valvesoftware.Steam/.local/share/OpenLamina. That is
chosen by itself when only Flatpak Steam is found; --flatpak or --native chooses.

Needs python3 (to edit the JSON file); --update also needs curl or wget, and sha256sum.
EOF
}

say() { printf '%s\n' "$*"; }
die() {
	printf 'install.sh: %s\n' "$*" >&2
	exit 1
}

command=install
target=auto
pre=0
while [ $# -gt 0 ]; do
	case $1 in
	--no-switch) command=copy ;;
	--openlamina | --opencomposite) command=use ;;
	--steamvr) command=steamvr ;;
	--uninstall) command=uninstall ;;
	--status) command=status ;;
	--update) command=update ;;
	--pre) pre=1 ;;
	--launch-options) command=launch ;;
	--flatpak) target=flatpak ;;
	--native) target=native ;;
	-h | --help)
		usage
		exit 0
		;;
	*)
		usage >&2
		exit 2
		;;
	esac
	shift
done
if [ $pre = 1 ] && [ $command != update ]; then
	die "--pre goes with --update."
fi
[ "$(id -u)" != 0 ] || die "this installs for one user: run it as your own user, not as root (no sudo)."

home=${HOME:?"HOME is not set"}
here=$(cd "$(dirname "$0")" && pwd -P)
flatpak_home=$home/.var/app/$FLATPAK_STEAM
if [ $target = auto ]; then
	if [ -d "$flatpak_home" ] && [ ! -d "$home/.steam/steam" ] && [ ! -d "${XDG_DATA_HOME:-$home/.local/share}/Steam" ]; then
		target=flatpak
	else
		target=native
	fi
fi
if [ $target = flatpak ]; then
	# The Flatpak keeps its home in ~/.var/app/<id>, which it also sees at that path.
	paths_file=$flatpak_home/.config/openvr/openvrpaths.vrpath
	copy=$flatpak_home/.local/share/OpenLamina
else
	paths_file=${XDG_CONFIG_HOME:-$home/.config}/openvr/openvrpaths.vrpath
	copy=${XDG_DATA_HOME:-$home/.local/share}/OpenLamina
fi
target_flag=--$target

# Both kinds of Steam: say how to reach the other one.
other_steam() {
	if [ $target = native ] && [ -d "$flatpak_home" ]; then
		say "Flatpak Steam is installed too; for it, run this again with --flatpak."
	fi
}

command -v python3 >/dev/null 2>&1 || die "python3 is needed (to edit $paths_file). Install it with your package manager."

# The JSON work: openvrpaths.vrpath's "runtime" list, and GitHub's release list. Every key of the paths file other than
# "runtime" is written back as it was read, in its order.
PY=$(
	cat <<'EOF'
import json, os, re, shlex, shutil, sys, time

def steamvr(path):
    return os.path.basename(path.rstrip("/")) == "SteamVR"

def same(a, b):
    return a.rstrip("/") == b.rstrip("/")

def load(file):
    if not os.path.exists(file):
        return None
    try:
        with open(file, encoding="utf-8") as f:
            data = json.load(f)
    except OSError as e:
        sys.exit(f"install.sh: can't read {file} ({e.strerror}).")
    except ValueError as e:
        sys.exit(f"install.sh: {file} isn't valid JSON ({e}); fix or delete it, then run this again.")
    runtime = data.get("runtime") if isinstance(data, dict) else None
    if not isinstance(data, dict) or not (runtime is None or isinstance(runtime, list) and all(isinstance(r, str) for r in runtime)):
        sys.exit(f"install.sh: {file} isn't an OpenVR paths file (\"runtime\" isn't a list of folders).")
    return data

# Written next to the real file (a symlink stays a symlink), with its permissions, after a backup named by the date and
# time (an earlier backup is never overwritten). A file that can't be written, as when made read-only to keep SteamVR
# from changing it, is refused and left as it is.
def save(file, data, old):
    file = os.path.realpath(file)
    folder = os.path.dirname(file)
    temp = file + ".install-tmp"
    try:
        os.makedirs(folder, exist_ok=True)
        if (old is not None and not os.access(file, os.W_OK)) or not os.access(folder, os.W_OK):
            raise PermissionError(None, None, file)
        if old is not None:
            stamp = time.strftime("%Y-%m-%d-%H%M%S")
            backup, n = f"{file}.{stamp}.bak", 1
            while os.path.exists(backup):
                n += 1
                backup = f"{file}.{stamp}-{n}.bak"
            shutil.copyfile(file, backup)
            print(f"Backed up {file} to {backup}")
        with open(temp, "w", encoding="utf-8") as f:
            json.dump(data, f, indent="\t", ensure_ascii=False)
            f.write("\n")
        if old is not None:
            shutil.copymode(file, temp)
        os.replace(temp, file)
    except PermissionError as e:
        if os.path.exists(temp):
            os.remove(temp)
        name = e.filename or file
        what = "file" if name == file else "folder"
        sys.exit(f"install.sh: {name} can't be written (it or its folder is read-only), so {'it' if name == file else file} was left as it is.\n"
                 f"To let install.sh change it, make the {what} writable (chmod u+w {shlex.quote(name)}) and run this again; "
                 f"SteamVR can then change it too. Or leave it, and choose the runtime per game with VR_OVERRIDE (README, \"Linux notes\").")

def version_key(v):
    m = re.fullmatch(r"(\d+(?:\.\d+)*)(?:-([0-9A-Za-z.-]+))?", v or "")
    if not m:
        return None
    numbers = [int(x) for x in m.group(1).split(".")]
    numbers += [0] * (8 - len(numbers))
    # Pre-release parts as semver orders them: numbers by value and before words, words by character code, and with
    # the shared parts equal, fewer parts first (rc.2 < rc.10 < rc.10.1; a release after its pre-releases).
    pre = [(0, int(x), "") if x.isdigit() else (1, 0, x) for x in (m.group(2) or "").split(".") if x]
    return (numbers, m.group(2) is None, pre)

def main(command, *args):
    if command == "status":
        file, ours = args
        data = load(file)
        runtimes = (data or {}).get("runtime") or []
        first = runtimes[0] if runtimes else ""
        kind = "none" if not first else "openlamina" if same(first, ours) else "steamvr" if steamvr(first) else "other"
        listed = any(same(r, ours) for r in runtimes)
        # "-" for no first runtime: the shell's read would merge an empty field with its tab neighbours
        print(f"{kind}\t{first or '-'}\t{'listed' if listed else 'unlisted'}\t{'exists' if data is not None else 'missing'}")
    elif command in ("use", "steamvr", "remove"):
        file, ours = args
        data = load(file)
        if data is None:
            if command != "use":
                return
            data = {"jsonid": "vrpathreg", "runtime": [], "version": 1}
            old = None
        else:
            old = data.get("runtime") or []
        runtimes = [r for r in (data.get("runtime") or []) if not same(r, ours)]
        if command == "use":
            runtimes.insert(0, ours)
        elif command == "steamvr" or (old and same(old[0], ours)):
            index = next((i for i, r in enumerate(runtimes) if steamvr(r)), None)
            if index:
                runtimes.insert(0, runtimes.pop(index))
        if old is not None and runtimes == old:
            return
        data["runtime"] = runtimes
        save(file, data, old)
    elif command == "source-version":
        (file,) = args
        try:
            with open(file, encoding="utf-8") as f:
                m = re.fullmatch(r"(?:OpenLamina|OpenComposite) (\S+), commit [0-9a-f]+", f.readline().strip())
        except OSError:
            m = None
        print(m.group(1) if m and version_key(m.group(1)) else "")
    elif command == "newest":
        # GitHub's release list on stdin: the newest release newer than the installed version (empty: none) with the
        # archive and its SHA-256, as "version<TAB>url<TAB>sha256".
        installed, pre, asset_name = args
        best = None
        for release in json.load(sys.stdin):
            if release.get("draft") or (release.get("prerelease") and pre != "1"):
                continue
            tag = release.get("tag_name") or ""
            version = tag[1:] if tag[:1] in ("v", "V") else tag
            key = version_key(version)
            if key is None or (installed and version_key(installed) and key <= version_key(installed)):
                continue
            for asset in release.get("assets") or []:
                digest = asset.get("digest") or ""
                if asset.get("name") == asset_name and re.fullmatch(r"sha256:[0-9a-fA-F]{64}", digest):
                    if best is None or key > best[0]:
                        best = (key, version, asset["browser_download_url"], digest[7:].lower())
        if best:
            print("\t".join(best[1:]))

main(*sys.argv[1:])
EOF
)
py() { python3 -c "$PY" "$@"; }

# What OpenVR games use now, tab-separated: kind (openlamina, the copy; steamvr; other; none), the first entry or
# '-', whether the copy is listed (listed/unlisted), and whether the paths file exists (exists/missing).
current() {
	py status "$paths_file" "$copy"
}

installed_version() {
	py source-version "$copy/SOURCE.txt"
}

# Copies this folder's OpenLamina to $copy. Each file goes to a temporary name first and is then renamed over the
# old one, so a game that has the old library loaded keeps running.
copy_files() {
	if [ ! -f "$here/bin/linux64/vrclient.so" ] || [ ! -f "$here/bin/vrclient.so" ]; then
		die "$here has no bin/linux64/vrclient.so and bin/vrclient.so: run install.sh from the unpacked $ARCHIVE."
	fi
	if [ "$here" = "$(cd "$copy" 2>/dev/null && pwd -P)" ]; then
		return # Run from the installed copy.
	fi
	mkdir -p "$copy/bin/linux64"
	for file in bin/linux64/vrclient.so bin/vrclient.so bin/version.txt SOURCE.txt install.sh; do
		put "$here/$file" "$copy/$file"
	done
	for file in "$here"/LICENSE*.txt "$here"/LICENCE*.txt "$here"/NOTES*.txt; do
		if [ -f "$file" ]; then
			put "$file" "$copy/${file##*/}"
		fi
	done
	say "Installed OpenLamina $(installed_version) in $copy"
}

put() {
	cp "$1" "$2.install-tmp"
	mv -f "$2.install-tmp" "$2"
}

use_openlamina() {
	[ -f "$copy/bin/linux64/vrclient.so" ] || die "OpenLamina isn't installed in $copy: run install.sh without options first."
	py use "$paths_file" "$copy"
	say "OpenVR games now use OpenLamina ($copy)."
	say "Starting SteamVR from Steam switches them back to SteamVR; $copy/install.sh --openlamina switches again."
	manager_hints
}

# steamvr: the copy out of the list and SteamVR first. remove (the uninstall): the copy out of the list, and SteamVR
# first only if the copy was.
use_steamvr() {
	py "$1" "$paths_file" "$copy"
	IFS='	' read -r kind first _ _ <<EOF
$(current)
EOF
	case $kind in
	steamvr) say "OpenVR games now use SteamVR ($first)." ;;
	none) say "OpenLamina isn't in $paths_file, which names no other runtime." ;;
	*) say "OpenLamina isn't in $paths_file; OpenVR games use $first." ;;
	esac
}

status() {
	# A paths file that can't be read: current says why, and nothing more is known about it.
	state=$(current) || state=unreadable
	IFS='	' read -r kind first listed exists <<EOF
$state
EOF
	say "OpenVR paths file: $paths_file$([ "$exists" = missing ] && printf ' (none yet)')"
	case $kind in
	openlamina) say "OpenVR games use: OpenLamina ($first)" ;;
	steamvr) say "OpenVR games use: SteamVR ($first)" ;;
	other) say "OpenVR games use: $first" ;;
	unreadable) say "OpenVR games use: unreadable (see above)" ;;
	*) say "OpenVR games use: nothing set" ;;
	esac
	if [ -f "$copy/bin/linux64/vrclient.so" ]; then
		version=$(installed_version)
		[ -n "$version" ] || version="(a build that is not a release)"
		say "Installed: OpenLamina $version in $copy$([ "$listed" = unlisted ] && printf ', not in the paths file')"
	else
		say "Installed: no ($copy)"
	fi
	runtime=$(openxr_runtime)
	if [ -n "$runtime" ]; then
		say "OpenXR runtime: $runtime"
	else
		say "OpenXR runtime: none found. OpenLamina needs your headset's OpenXR runtime to be the active one."
	fi
}

# The active OpenXR runtime's manifest, as the OpenXR loader finds it outside the Steam Runtime.
openxr_runtime() {
	if [ -n "${XR_RUNTIME_JSON:-}" ]; then
		say "$XR_RUNTIME_JSON"
		return
	fi
	dirs="${XDG_CONFIG_HOME:-$home/.config}:${XDG_CONFIG_DIRS:-/etc/xdg}:/etc"
	old_ifs=$IFS
	IFS=:
	for dir in $dirs; do
		if [ -e "$dir/openxr/1/active_runtime.json" ]; then
			IFS=$old_ifs
			say "$dir/openxr/1/active_runtime.json"
			return
		fi
	done
	IFS=$old_ifs
}

# What a game in the Steam Runtime (most Proton games) needs to reach the OpenXR runtime: pressure-vessel imports the
# host's active runtime, its manifest and library, when asked to.
launch_options() {
	manifest=$(openxr_runtime)
	if [ -z "$manifest" ]; then
		say "No active OpenXR runtime found: set your headset's runtime as the active one (its own app or docs say how)."
		return
	fi
	resolved=$(readlink -f "$manifest" || printf '%s' "$manifest")
	say "OpenXR runtime: $manifest$([ "$resolved" != "$manifest" ] && printf ' -> %s' "$resolved")"
	say "Games in the Steam Runtime (most Proton games) need this launch option (Steam: the game's Properties,"
	say "General, Launch options) to reach it:"
	say ""
	say "    PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1 %command%"
	if [ $target = flatpak ]; then
		say ""
		say "Flatpak Steam: the runtime has to be visible inside the Flatpak as well (one installed for Flatpak Steam,"
		say "or the Flatpak given access to its files)."
	fi
	say ""
	say "The README's \"Linux notes\" explain more."
}

# A VR server that writes openvrpaths.vrpath itself when it starts puts its own OpenVR runtime first again: say how to
# make it choose OpenLamina.
manager_hints() {
	config=${XDG_CONFIG_HOME:-$home/.config}
	listed=$(cat "$paths_file" 2>/dev/null || true)
	case $listed in
	*[Ww]i[Vv][Rr]n*) wivrn=1 ;;
	*) wivrn=0 ;;
	esac
	if [ $wivrn = 1 ] || [ -d "$config/wivrn" ] || [ -d "$home/.var/app/io.github.wivrn.wivrn" ]; then
		say "WiVRn writes $paths_file when it starts, with its own OpenVR runtime first. To make it choose OpenLamina,"
		say "set \"openvr-compat-path\": \"$copy\" in its config.json ($config/wivrn/config.json, or in the"
		say "Flatpak's ~/.var/app/io.github.wivrn.wivrn/config/wivrn/)."
	fi
	case $listed in
	*[Ee]nvision*) envision=1 ;;
	*) envision=0 ;;
	esac
	if [ $envision = 1 ] || [ -d "$config/envision" ] || [ -d "${XDG_DATA_HOME:-$home/.local/share}/envision" ]; then
		say "Envision writes $paths_file when it starts a profile, with the profile's OpenVR runtime first. To make it"
		say "choose OpenLamina, set that profile's OpenVR runtime path to $copy."
	fi
}

# Files the owner put in the copy's bin/ (openlamina.ini, say), moved to OpenLamina-kept-<date> next to the copy before
# the copy is deleted. Prints where, or nothing when there were none.
keep_own_files() {
	[ -d "$copy/bin" ] || return 0
	rm -f "$copy/bin/linux64/vrclient.so" "$copy/bin/vrclient.so" "$copy/bin/version.txt"
	find "$copy/bin" -depth -type d -empty -delete
	[ -d "$copy/bin" ] || return 0
	aside=$copy-kept-$(date +%Y-%m-%d-%H%M%S)
	mkdir -p "$aside"
	mv "$copy/bin" "$aside/bin"
	say "Kept the files you put in $copy/bin: they are in $aside/bin."
}

fetch() {
	if command -v curl >/dev/null 2>&1; then
		curl -fsSL --proto =https --proto-redir =https -H "User-Agent: OpenLamina-installer" "$1"
	elif command -v wget >/dev/null 2>&1; then
		wget -q -O - --header "User-Agent: OpenLamina-installer" "$1"
	else
		die "--update needs curl or wget."
	fi
}

update() {
	command -v sha256sum >/dev/null 2>&1 || die "--update needs sha256sum (coreutils)."
	installed=
	[ -f "$copy/bin/linux64/vrclient.so" ] && installed=$(installed_version)
	releases=$(fetch "https://api.github.com/repos/$REPOSITORY/releases?per_page=30") ||
		die "Couldn't ask GitHub for the releases."
	newest=$(printf '%s' "$releases" | py newest "$installed" "$pre" "$ARCHIVE")
	if [ -z "$newest" ]; then
		say "No newer release${installed:+ than $installed}$([ $pre = 0 ] && printf ' (--pre includes pre-releases)')."
		return
	fi
	IFS='	' read -r version url sha <<EOF
$newest
EOF
	say "Downloading OpenLamina $version"
	work=$(mktemp -d)
	trap 'rm -rf "$work"' EXIT
	# sh runs the EXIT trap on exit, not when killed by a signal.
	trap 'exit 130' INT TERM HUP
	fetch "$url" >"$work/$ARCHIVE" || die "Couldn't download $url."
	actual=$(sha256sum "$work/$ARCHIVE" | cut -d' ' -f1)
	[ "$actual" = "$sha" ] || die "$ARCHIVE was refused: its SHA-256 is $actual, but GitHub lists $sha."
	tar -xzf "$work/$ARCHIVE" -C "$work"
	sh "$work/OpenLamina/install.sh" --no-switch "$target_flag"
}

case $command in
install)
	copy_files
	use_openlamina
	other_steam
	say ""
	launch_options
	;;
copy) copy_files ;;
use) use_openlamina ;;
steamvr) use_steamvr steamvr ;;
uninstall)
	use_steamvr remove
	if [ -d "$copy" ]; then
		keep_own_files
		rm -rf "$copy"
		say "Deleted $copy."
	fi
	;;
status)
	status
	other_steam
	manager_hints
	;;
update) update ;;
launch) launch_options ;;
esac
