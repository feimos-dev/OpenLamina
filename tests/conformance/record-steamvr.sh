#!/bin/sh
# Records SteamVR's own answers on Linux, as record-steamvr.ps1 does on Windows (README.md, "The SteamVR reference"):
# with SteamVR's null headset (--controllers: real controllers too; --devices: the drivers as they are), and writes
# expected/steamvr-linux-<version>.json, or with --cases only those cases' results. Every file SteamVR may change is
# backed up first and put back at the end, and the run fails unless each SHA-256 matches its copy.
#
#   tests/conformance/record-steamvr.sh --conformance <oc_conformance> --out <dir> [--controllers | --devices]
#       [--cases <a,b,...>] [--resume] [--jobs <n>] [--what-if]
set -eu

here=$(cd "$(dirname "$0")" && pwd)
conformance= out= mode=null cases= resume= jobs=1 whatif=
while [ $# -gt 0 ]; do
	case $1 in
	--conformance) conformance=$2; shift 2 ;;
	--out) out=$2; shift 2 ;;
	--controllers) mode=controllers; shift ;;
	--devices) mode=devices; shift ;;
	--cases) cases=$2; shift 2 ;;
	--resume) resume=--resume; shift ;;
	--jobs) jobs=$2; shift 2 ;;
	--what-if) whatif=1; shift ;;
	*) echo "record-steamvr.sh: unknown argument $1" >&2; exit 2 ;;
	esac
done
[ -n "$conformance" ] && [ -n "$out" ] || { sed -n '2,8p' "$0" >&2; exit 2; }
conformance=$(cd "$(dirname "$conformance")" && pwd)/$(basename "$conformance")
mkdir -p "$out"
out=$(cd "$out" && pwd)

# 1. SteamVR and Steam's config folder
paths=${XDG_CONFIG_HOME:-$HOME/.config}/openvr/openvrpaths.vrpath
steamvr=
if [ -f "$paths" ]; then
	steamvr=$(python3 -I -c 'import json, os, sys
for r in json.load(open(sys.argv[1])).get("runtime") or []:
    if os.path.basename(os.path.normpath(r)) == "SteamVR" and os.path.isfile(os.path.join(r, "bin/linux64/vrclient.so")):
        print(r); break' "$paths")
fi
steam=${STEAM_DIR:-$HOME/.local/share/Steam}
if [ -z "$steamvr" ] && [ -f "$steam/steamapps/common/SteamVR/bin/linux64/vrclient.so" ]; then
	steamvr=$steam/steamapps/common/SteamVR
fi
[ -n "$steamvr" ] || { echo "record-steamvr.sh: SteamVR not found" >&2; exit 1; }
config=$steam/config
case "$HOME$steam" in *[[:space:]]*) echo "record-steamvr.sh: a folder name with a space isn't supported (its backup list)" >&2; exit 1 ;; esac
[ -d "$config" ] || { echo "record-steamvr.sh: Steam's config folder $config not found (STEAM_DIR)" >&2; exit 1; }
version=$(tr -d '[:space:]' <"$steamvr/bin/version.txt")
echo "SteamVR $version at $steamvr; Steam config $config; drivers: $mode"

# 2. Nothing VR running
for p in vrserver vrmonitor vrcompositor monado-service; do
	if pgrep -x "$p" >/dev/null; then
		echo "record-steamvr.sh: close $p first" >&2
		exit 1
	fi
done

# 3. The backup
runtime=${XDG_CONFIG_HOME:-$HOME/.config}/openxr/1/active_runtime.json
backup=$out/backup
manifest=$backup/manifest.txt
if [ -f "$manifest" ]; then
	echo "record-steamvr.sh: an earlier run's backup was never put back: see $manifest (each line: kind, SHA-256, path, copy)" >&2
	exit 1
fi
files="$paths $config/steamvr.vrsettings $config/chaperone_info.vrchap $config/appconfig.json"
if [ -d "$config/vrappconfig" ]; then
	for f in "$config"/vrappconfig/*; do [ -f "$f" ] && files="$files $f"; done
fi
if [ -n "$whatif" ]; then
	for f in $files $runtime; do echo "would back up: $f"; done
	echo "would run run.py against $steamvr/bin/linux64/vrclient.so, then put everything back"
	exit 0
fi
mkdir -p "$backup"
: >"$manifest.new"
n=0
for f in $files $runtime; do
	n=$((n + 1))
	if [ -L "$f" ]; then
		# Kept as a link, and the file it points to as a file: SteamVR may write through the link
		echo "link $(readlink "$f") $f -" >>"$manifest.new"
		f=$(readlink -f "$f")
		[ -f "$f" ] || continue
	fi
	if [ -f "$f" ]; then
		cp -p "$f" "$backup/$n"
		echo "file $(sha256sum <"$f" | cut -d' ' -f1) $f $backup/$n" >>"$manifest.new"
	else
		echo "none - $f -" >>"$manifest.new"
	fi
done
ls "$config/vrappconfig" 2>/dev/null >"$backup/vrappconfig.before" || : >"$backup/vrappconfig.before"
mv "$manifest.new" "$manifest"

stop_steamvr() {
	# Steam's launch of SteamVR first (it starts it again otherwise), then SteamVR's own programs
	pkill -f 'AppId=250820' 2>/dev/null || true
	for round in 1 2 3; do
		for p in vrmonitor vrserver vrcompositor vrdashboard vrwebhelper vrstartup; do pkill -x "$p" 2>/dev/null || true; done
		i=0
		while [ $i -lt 30 ] && pgrep -x 'vrserver|vrmonitor|vrcompositor' >/dev/null; do sleep 1; i=$((i + 1)); done
		pgrep -x 'vrserver|vrmonitor|vrcompositor' >/dev/null || return 0
		for p in vrmonitor vrserver vrcompositor vrdashboard vrwebhelper; do pkill -KILL -x "$p" 2>/dev/null || true; done
		sleep 3
	done
	return 1
}

restore() {
	status=$?
	trap '' INT TERM
	set +e
	mismatch=
	stop_steamvr || mismatch="$mismatch SteamVR-still-running"
	while read -r kind hash path copy; do
		case $kind in
		file)
			cp -p "$copy" "$path"
			[ "$(sha256sum <"$path" | cut -d' ' -f1)" = "$hash" ] || mismatch="$mismatch $path"
			;;
		link)
			[ "$(readlink "$path" 2>/dev/null)" = "$hash" ] || { rm -f "$path"; ln -s "$hash" "$path"; }
			;;
		none) rm -f "$path" ;;
		esac
	done <"$manifest"
	if [ -d "$config/vrappconfig" ]; then
		for f in "$config"/vrappconfig/*; do
			[ -f "$f" ] || continue
			grep -qxF "$(basename "$f")" "$backup/vrappconfig.before" || { echo "removing $f, made during the run"; rm -f "$f"; }
		done
	fi
	if [ -n "$mismatch" ]; then
		echo "record-steamvr.sh: NOT put back exactly (backups in $backup):$mismatch" >&2
		exit 1
	fi
	mv "$manifest" "$backup/manifest.restored.txt"
	echo "Every file put back; SHA-256s match ($backup)."
	exit "$status"
}
trap restore EXIT
trap 'exit 130' INT TERM

# 4. The drivers, and SteamVR first in the paths file
python3 -I - "$config/steamvr.vrsettings" "$paths" "$steamvr" "$mode" <<'EOF'
import json, os, sys
settings_path, paths_path, steamvr, mode = sys.argv[1:]
settings = json.load(open(settings_path)) if os.path.exists(settings_path) else {}
if mode in ("null", "controllers"):
    settings.setdefault("driver_null", {})["enable"] = True
    settings.setdefault("steamvr", {})["forcedDriver"] = "null"
    settings["steamvr"]["activateMultipleDrivers"] = mode == "controllers"
if mode in ("controllers", "devices"):
    power = settings.setdefault("power", {})
    power["powerOffOnExit"] = False
    power["turnOffControllersTimeout"] = 86400.0  # seconds: a day
json.dump(settings, open(settings_path, "w"), indent=3)
if os.path.exists(paths_path):
    paths = json.load(open(paths_path))
    paths["runtime"] = [steamvr] + [r for r in paths.get("runtime") or [] if os.path.normpath(r) != os.path.normpath(steamvr)]
    json.dump(paths, open(paths_path, "w"), indent=1)
EOF

# 5. The run
set +e
python3 -I "$here/run.py" --runtime "$steamvr/bin/linux64/vrclient.so" --conformance "$conformance" --xr-runtime none \
	--real-runtime --keep-running steam://run/250820 --allow-failures --jobs "$jobs" --out "$out" $resume ${cases:+--cases "$cases"}
code=$?
set -e
[ $code -eq 0 ] || { echo "record-steamvr.sh: run.py failed ($code)" >&2; exit 1; }

# 6. The reference; the EXIT trap then puts the files back
if [ -z "$cases" ]; then
	python3 -I "$here/reference.py" "$out/results.json" "$here/expected/steamvr-linux-$version.json"
else
	echo "Results of $cases in $out/results.json; no reference written."
fi
