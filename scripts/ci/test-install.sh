#!/bin/sh
# Tries the Linux archive's install.sh on a scratch home folder: install, switch both ways, keep the owner's files,
# uninstall (moving the owner's files aside), dated backups, a read-only paths file refused, and the paths file's other
# keys kept throughout. No network, no Steam.
#
#   scripts/ci/test-install.sh dist/OpenLamina-Linux.tar.gz
set -eu

archive=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
fail() {
	echo "test-install.sh: $*" >&2
	exit 1
}

mkdir -p "$work/unpacked" "$work/home folder/.config/openvr" "$work/home folder/.local/share/Steam"
tar -xzf "$archive" -C "$work/unpacked"
installer=$work/unpacked/OpenLamina/install.sh
[ -x "$installer" ] || fail "the archive has no executable OpenLamina/install.sh."

export HOME="$work/home folder"
unset XDG_CONFIG_HOME XDG_DATA_HOME XDG_CONFIG_DIRS XR_RUNTIME_JSON
paths=$HOME/.config/openvr/openvrpaths.vrpath
copy=$HOME/.local/share/OpenLamina
steamvr=$HOME/.local/share/Steam/steamapps/common/SteamVR
cat >"$paths" <<EOF
{
	"config" : [ "$HOME/.local/share/Steam/config" ],
	"external_drivers" : null,
	"jsonid" : "vrpathreg",
	"log" : [ "$HOME/.local/share/Steam/logs" ],
	"runtime" : [ "$steamvr", "/opt/other-runtime" ],
	"version" : 1
}
EOF
cp "$paths" "$work/original"

# runtime_list: the "runtime" entries, one per line. same_keys: every key but "runtime" as in the original.
runtime_list() {
	python3 -c 'import json, sys; print("\n".join(json.load(open(sys.argv[1]))["runtime"]))' "$paths"
}
same_keys() {
	python3 -c '
import json, sys
a, b = (json.load(open(f)) for f in sys.argv[1:])
a.pop("runtime"); b.pop("runtime")
sys.exit(a != b)' "$work/original" "$paths" || fail "a key other than \"runtime\" changed."
}
expect() {
	actual=$(runtime_list)
	[ "$actual" = "$1" ] || fail "after $2, \"runtime\" is
$actual
and not
$1"
	same_keys
}

sh "$installer" >"$work/log" || fail "install failed: $(cat "$work/log")"
for file in bin/linux64/vrclient.so bin/vrclient.so bin/version.txt SOURCE.txt install.sh LICENSE.txt; do
	[ -f "$copy/$file" ] || fail "the install has no $file."
done
expect "$copy
$steamvr
/opt/other-runtime" "install"
set -- "$paths".*.bak
if [ $# != 1 ] || ! cmp -s "$1" "$work/original"; then
	fail "the dated backup of openvrpaths.vrpath isn't the file as it was."
fi
case $(cat "$work/log") in
*"PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1 %command%"* | *"No active OpenXR runtime found"*) ;;
*) fail "install printed no launch options: $(cat "$work/log")" ;;
esac

status=$(sh "$copy/install.sh" --status)
case $status in
*"OpenVR games use: OpenLamina"*) ;;
*) fail "--status doesn't say OpenLamina: $status" ;;
esac

echo "haptics=off" >"$copy/bin/openlamina.ini"
echo "haptics=off" >"$copy/bin/opencomposite.ini"
sh "$installer" --no-switch >/dev/null
[ -f "$copy/bin/openlamina.ini" ] || fail "installing again deleted bin/openlamina.ini."
[ -f "$copy/bin/opencomposite.ini" ] || fail "installing again deleted bin/opencomposite.ini."

sh "$copy/install.sh" --steamvr >/dev/null
expect "$steamvr
/opt/other-runtime" "--steamvr"
set -- "$paths".*.bak
[ $# = 2 ] || fail "--steamvr didn't make a second backup (or overwrote the first): $*"

# A read-only paths file is refused, and left as it is (root can write to it all the same)
if [ "$(id -u)" != 0 ]; then
	chmod a-w "$paths"
	cp "$paths" "$work/read-only"
	if sh "$copy/install.sh" --openlamina >"$work/log" 2>&1; then
		fail "--openlamina changed a read-only paths file."
	fi
	grep -q "chmod u+w" "$work/log" || fail "the refusal doesn't say how to make the file writable: $(cat "$work/log")"
	cmp -s "$paths" "$work/read-only" || fail "the read-only paths file changed."
	chmod u+w "$paths"
	# A read-only folder too: the message names the folder
	chmod a-w "$(dirname "$paths")"
	if sh "$copy/install.sh" --openlamina >"$work/log" 2>&1; then
		fail "--openlamina changed the paths file in a read-only folder."
	fi
	grep -qF "make the folder writable" "$work/log" || fail "the refusal doesn't name the folder: $(cat "$work/log")"
	chmod u+w "$(dirname "$paths")"
fi

sh "$copy/install.sh" --openlamina >/dev/null
expect "$copy
$steamvr
/opt/other-runtime" "--openlamina"
sh "$copy/install.sh" --steamvr >/dev/null
sh "$copy/install.sh" --opencomposite >/dev/null
expect "$copy
$steamvr
/opt/other-runtime" "--opencomposite, the old name of --openlamina"

sh "$copy/install.sh" --uninstall >/dev/null
expect "$steamvr
/opt/other-runtime" "--uninstall"
[ ! -e "$copy" ] || fail "--uninstall left $copy."
set -- "$copy"-kept-*
if [ ! -f "$1/bin/openlamina.ini" ] || [ ! -f "$1/bin/opencomposite.ini" ]; then
	fail "--uninstall didn't keep the files put in bin/."
fi
[ ! -e "$1/bin/linux64/vrclient.so" ] || fail "--uninstall kept its own library with the owner's files."

echo "install.sh: install, status, reinstall, --steamvr, --openlamina and --uninstall work."
