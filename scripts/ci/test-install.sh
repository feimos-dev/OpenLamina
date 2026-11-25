#!/bin/sh
# Tries the Linux archive's install.sh on a scratch home folder: install, switch both ways, keep the owner's files,
# uninstall (moving the owner's files aside), dated backups, a read-only paths file refused, and the paths file's other
# keys kept throughout; --update with a stand-in curl and checksums signed with a key made for the test. No network,
# no Steam. Needs openssl.
#
#   scripts/ci/test-install.sh dist/OpenLamina-Linux.tar.gz
set -eu

archive=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
work=$(mktemp -d)
trap 'chmod -R u+w "$work" 2>/dev/null; rm -rf "$work"' EXIT
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
for file in bin/linux64/vrclient.so bin/vrclient.so bin/version.txt SOURCE.txt install.sh LICENSE.txt LICENSE_webxr-input-profiles.txt bin/rendermodels/valve-index/valve-index-left.json; do
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
[ ! -e "$1/bin/rendermodels" ] || fail "--uninstall kept its own controller models with the owner's files."

# Without files of the owner's, nothing is kept
sh "$installer" --no-switch >/dev/null
sh "$copy/install.sh" --uninstall >"$work/log"
[ ! -e "$copy" ] || fail "the second --uninstall left $copy."
if grep -q "Kept the files" "$work/log"; then
	fail "--uninstall without files of the owner's kept some: $(cat "$work/log")"
fi

# --update against a release list, archive and signed checksums served by a stand-in curl: the address must be on
# github.com, the checksums signed with the release key and listing the archive's SHA-256, and the newest version is
# chosen. The key is one made for the test, put in a copy of install.sh in place of the pinned one.
mkdir -p "$work/stub"
cat >"$work/stub/curl" <<'EOF'
#!/bin/sh
for url; do :; done
case $url in
https://api.github.com/*) cat "$STUB_RELEASES" ;;
https://github.com/*/SHA256SUMS.txt) cat "$STUB_SUMS" ;;
https://github.com/*/SHA256SUMS.txt.sig) cat "$STUB_SIG" ;;
https://github.com/*) cat "$STUB_ARCHIVE" ;;
*) exit 22 ;;
esac
EOF
chmod +x "$work/stub/curl"
openssl ecparam -name prime256v1 -genkey -noout -out "$work/test-key.pem" 2>/dev/null || fail "openssl couldn't make a test key."
openssl ec -in "$work/test-key.pem" -pubout -out "$work/test-key.pub.pem" 2>/dev/null || fail "openssl couldn't write the test key's public half."
openssl ecparam -name prime256v1 -genkey -noout -out "$work/other-key.pem" 2>/dev/null || fail "openssl couldn't make a second test key."
# with_key <public key file or -> <out>: a copy of install.sh pinning that key (-: the placeholder)
with_key() {
	python3 - "$installer" "$1" "$2" <<'EOF'
import re, sys
src, key, out = sys.argv[1:]
text = open(src, encoding="utf-8").read()
value = "REPLACE-WITH-THE-RELEASE-PUBLIC-KEY" if key == "-" else open(key, encoding="utf-8").read().strip()
text, n = re.subn(r"(?m)^RELEASE_KEY='[^']*'", lambda m: "RELEASE_KEY='" + value + "'", text, count=1)
if n != 1:
    sys.exit("install.sh has no RELEASE_KEY='...' line")
open(out, "w", encoding="utf-8").write(text)
EOF
}
with_key "$work/test-key.pub.pem" "$work/install-test-key.sh" || fail "couldn't pin the test key in a copy of install.sh."
with_key - "$work/install-no-key.sh" || fail "couldn't put the placeholder in a copy of install.sh."
export STUB_RELEASES="$work/releases.json" STUB_ARCHIVE="$archive" STUB_SUMS="$work/SHA256SUMS.txt" STUB_SIG="$work/SHA256SUMS.txt.sig"
hex=$(sha256sum "$archive" | cut -d' ' -f1)
sha=sha256:$hex
zero=0000000000000000000000000000000000000000000000000000000000000000
good_url=https://github.com/feimos-dev/OpenLamina/releases/download/v99999.0.0/OpenLamina-Linux.tar.gz
# sign [<key> [<tag>]]: STUB_SIG made from STUB_SUMS as the release job makes it, with the test key (or <key>), for
# v99999.0.0 (or <tag>)
sign() {
	{ printf 'OpenLamina %s\n' "${2:-v99999.0.0}" && cat "$STUB_SUMS"; } >"$work/signed"
	openssl dgst -sha256 -sign "${1:-$work/test-key.pem}" -out "$STUB_SIG" "$work/signed" || fail "openssl couldn't sign the test checksums."
}
# sums <sha256 of the archive> [<key> [<tag>]]: SHA256SUMS.txt listing it, signed
sums() {
	printf '%s  OpenLamina-Source.tar.gz\n%s  OpenLamina-Linux.tar.gz\n' "$zero" "$1" >"$STUB_SUMS"
	sign "${2:-}" "${3:-}"
}
# releases <url or -> <digest> <tag> <prerelease> [<tag> <prerelease> ...]: one release per tag, each with the archive
# and, unless $unsigned is 1, SHA256SUMS.txt and its signature
releases() {
	UNSIGNED=$unsigned python3 -c '
import json, os, sys
url, digest, rest = sys.argv[1], sys.argv[2], sys.argv[3:]
out = []
for tag, pre in zip(rest[::2], rest[1::2]):
    asset = {"name": "OpenLamina-Linux.tar.gz", "digest": digest, "size": 1}
    if url != "-":
        asset["browser_download_url"] = url
    assets = [asset]
    if os.environ.get("UNSIGNED") != "1":
        for name in ("SHA256SUMS.txt", "SHA256SUMS.txt.sig"):
            assets.append({"name": name, "size": 1,
                           "browser_download_url": "https://github.com/feimos-dev/OpenLamina/releases/download/" + tag + "/" + name})
    out.append({"tag_name": tag, "prerelease": pre == "true", "draft": False, "assets": assets})
print(json.dumps(out))' "$@" >"$STUB_RELEASES"
}
# update [--pre]: $updater --update
updater=$work/install-test-key.sh
unsigned=0
update() {
	PATH="$work/stub:$PATH" sh "$updater" --update "$@" >"$work/log" 2>&1
}
# refused <what> <message>: --update fails, says <message>, and installs nothing
refused() {
	if update; then
		fail "--update installed $1."
	fi
	grep -Eq "$2" "$work/log" || fail "--update didn't say why it refused $1: $(cat "$work/log")"
	[ ! -e "$copy" ] || fail "--update installed $1."
}

sums "$hex"
releases - "$sha" v99999.0.0 false
update || fail "--update with a release without an address failed: $(cat "$work/log")"
grep -q "No newer release" "$work/log" || fail "--update took a release without an address: $(cat "$work/log")"
releases http://example.com/OpenLamina-Linux.tar.gz "$sha" v99999.0.0 false
update || fail "--update with a release elsewhere failed: $(cat "$work/log")"
grep -q "No newer release" "$work/log" || fail "--update took a release that isn't on github.com: $(cat "$work/log")"
releases "$good_url" "$sha" v99999.0.0 false
updater=$work/install-no-key.sh
refused "a release with no release key pinned" "no release key"
# The archive's own install.sh: its key (or, before the key is made, none) didn't sign the test's checksums
updater=$installer
refused "a release the test's key signed, with the installer's own key" "no release key|isn't signed for"
updater=$work/install-test-key.sh
unsigned=1
releases "$good_url" "$sha" v99999.0.0 false
refused "a release without signed checksums" "has no signed checksums"
unsigned=0
releases "$good_url" "$sha" v99999.0.0 false
sums "$hex" "$work/other-key.pem"
refused "a release whose checksums another key signed" "isn't signed for v99999.0.0 with"
sums "$hex" "" v99998.0.0
refused "another release's signed checksums" "isn't signed for v99999.0.0 with"
sums "$hex"
printf 'changed\n' >>"$STUB_SUMS"
refused "a release whose checksums changed after signing" "isn't signed for v99999.0.0 with"
sums "$zero"
refused "a release whose signed checksums list another SHA-256 than GitHub" "GitHub lists another SHA-256"
releases "$good_url" sha256:$zero v99999.0.0 false
refused "an archive whose SHA-256 isn't the signed one" "was refused: its SHA-256 is"
printf '%s  OpenLamina-Source.tar.gz\n%s *OpenLamina-Linux.tar.gz\n' "$hex" "$hex" >"$STUB_SUMS"
sign
releases "$good_url" "$sha" v99999.0.0 false
refused "an archive the signed checksums don't list" "doesn't list OpenLamina-Linux.tar.gz"
sums "$hex" "" v99998.0.0
releases "$good_url" "$sha" v99999.0.0-rc.2 true v99999.0.0-rc.10 true v99998.0.0 false
update || fail "--update failed: $(cat "$work/log")"
grep -q "Downloading OpenLamina 99998.0.0" "$work/log" || fail "--update didn't choose 99998.0.0: $(cat "$work/log")"
sums "$hex" "" v99999.0.0-rc.10
update --pre || fail "--update --pre failed: $(cat "$work/log")"
grep -q "Downloading OpenLamina 99999.0.0-rc.10" "$work/log" || fail "--update --pre didn't choose 99999.0.0-rc.10: $(cat "$work/log")"
[ -f "$copy/bin/linux64/vrclient.so" ] || fail "--update didn't install the archive."
expect "$steamvr
/opt/other-runtime" "--update, which leaves the switch as it is"

echo "install.sh: install, status, reinstall, --steamvr, --openlamina, --uninstall and --update work."
