#!/usr/bin/env bash
# Install, launch and feed Vice City VR on a Valve Steam Frame (Android in the
# Lepton container) from a Mac or Linux host. See docs/STEAM_FRAME.md.
#
#   tools/steam_frame/deploy.sh [options]
#
#   --apk FILE         the Frame APK (default: ~/VCVRFrameBuild/tree's
#                      assembleFrame output)
#   --tree DIR         assembled tree (default ~/VCVRFrameBuild/tree); its
#                      gamefiles/ holds the port's text, frontend and hand assets
#   --game-dir DIR     your GTA Vice City PC installation: copies data TEXT anim
#                      txd skins mp3 movies models Audio, then the port's assets,
#                      to the headset's ~/Documents/ViceCityVR/gamedata (rsync
#                      over SSH; about 1.5 GB the first time)
#   --from-steam       the same, from the Steam PC version installed on the
#                      Frame itself (app 12110, steamapps/common/Grand Theft
#                      Auto Vice City); the copy runs on the headset
#   --port-assets      copy only the port's assets (text, frontend, VR hands)
#   --register-title   register ~/devkit-game/ViceCityVR as a persistent Steam
#                      library title (once). When the title exists, every
#                      install also refreshes its copy of the APK.
#   --no-install       skip adb install
#   --no-launch        skip starting the game over adb
#   --logcat SECONDS   after launching, capture SECONDS of log into
#                      scratchpad/frame-logcat.txt and print the key lines
#   --diag KEY=VAL     set a key in ~/Documents/ViceCityVR/vr-diag.ini
#                      (repeatable): force_render, diag_yaw, dump_frame, timing.
#                      --diag-reset deletes the file.
#   --clear-dumps      remove ~/Documents/ViceCityVR/vr-dumps before the run
#   --pull-dumps DIR   after the run, copy vr-dumps into DIR (PNG copies with
#                      sips on a Mac)
#   --no-adb           only SSH steps (data, title); no Lepton Development needed
#
# Environment:
#   FRAME_HOST       ssh target (default steamos@192.168.1.31, the Frame on
#                    Wi-Fi; over USB networking use steamos@10.86.200.233)
#   FRAME_ADB_HOST   adb host (default: the host part of FRAME_HOST). Lepton
#                    Development publishes its adbd on <host>:5555
#   ADB              adb binary
#
# adb needs "Lepton Development" running (start it from the Frame's Steam
# library). It deletes its apps and their data when it exits; the persistent
# Steam title (--register-title) and ~/Documents/ViceCityVR survive. A game
# started over adb gets no controllers and no focus from SteamVR: use it for
# logs, and launch the ViceCityVR title from the Steam library to play.
#
# Before an unattended run (--diag, --logcat), make sure nobody is wearing the
# headset: a launch replaces whatever they are playing.
set -euo pipefail

kit="$(cd "$(dirname "$0")/../.." && pwd)"
package="com.miamivr.quest.frame"
activity="android.app.NativeActivity"
title="ViceCityVR"               # Steam devkit titles must be alphanumeric
title_apk="ViceCity-VR.apk"      # fixed name: the title's argv stays valid across builds
data="Documents/ViceCityVR"      # relative to the Frame user's home; /sdcard/Documents/ViceCityVR in Lepton

frame_host="${FRAME_HOST:-steamos@192.168.1.31}"
adb_host="${FRAME_ADB_HOST:-${frame_host#*@}}"
adb_bin="${ADB:-${ANDROID_HOME:-$HOME/Library/Android/sdk}/platform-tools/adb}"
[[ -x "$adb_bin" ]] || adb_bin="$(command -v adb || true)"
tree="$HOME/VCVRFrameBuild/tree"
apk=""
game_dir=""
port_assets=0
from_steam=0
register=0
install=1
launch=1
use_adb=1
logcat_seconds=0
diag=()
diag_reset=0
clear_dumps=0
pull_dumps=""

die() { echo "deploy.sh: $*" >&2; exit 1; }

while (($#)); do
	case "$1" in
	--apk) apk="$2"; shift 2 ;;
	--tree) tree="$2"; shift 2 ;;
	--game-dir) game_dir="$2"; shift 2 ;;
	--from-steam) from_steam=1; shift ;;
	--port-assets) port_assets=1; shift ;;
	--register-title) register=1; shift ;;
	--no-install) install=0; shift ;;
	--no-launch) launch=0; shift ;;
	--logcat) logcat_seconds="$2"; shift 2 ;;
	--diag) diag+=("$2"); shift 2 ;;
	--diag-reset) diag_reset=1; shift ;;
	--clear-dumps) clear_dumps=1; shift ;;
	--pull-dumps) pull_dumps="$2"; shift 2 ;;
	--no-adb) use_adb=0; shift ;;
	-h|--help) sed -n '2,/^set -euo/p' "$0" | sed '$d'; exit 0 ;;
	*) die "unknown option $1" ;;
	esac
done
[[ -n "$apk" ]] || apk="$tree/android/app/build/outputs/apk/frame/app-frame.apk"

ssh_frame() { ssh -o ConnectTimeout=10 -o BatchMode=yes "$frame_host" "$@"; }

# ---- Game data: persistent storage on the Frame host ------------------------
copy_port_assets() {
	local files="$tree/gamefiles"
	[[ -f "$files/TEXT/american.gxt" && -f "$files/models/x360btns.txd" ]] \
		|| die "$files lacks the port's assets (assemble the tree with tools/steam_frame/build.sh)"
	[[ -f "$files/models/vrhands/BigHandLeft.uxrh" ]] \
		|| die "$files/models/vrhands is missing the VR hand assets"
	# reVC's replacement text and frontend art, and the VR hands, over the
	# retail files (the Quest installer's order)
	rsync -rt --chmod=Du=rwx,Dgo=rwx,Fu=rw,Fgo=rw "$files/TEXT/" "$frame_host:$data/gamedata/TEXT/"
	rsync -rt --chmod=Du=rwx,Dgo=rwx,Fu=rw,Fgo=rw "$files/models/" "$frame_host:$data/gamedata/models/"
	echo "port assets in ~/$data/gamedata (TEXT, models, models/vrhands)"
}

if [[ -n "$game_dir" ]]; then
	[[ -d "$game_dir" ]] || die "no such folder: $game_dir"
	required=(data TEXT anim txd skins mp3 movies models Audio)
	src=()
	for name in "${required[@]}"; do
		# retail installs differ in case (Audio/audio, TEXT/Text)
		found="$(find "$game_dir" -maxdepth 1 -type d -iname "$name" | head -1)"
		[[ -n "$found" ]] || die "$game_dir has no $name folder: is it a GTA Vice City PC installation?"
		src+=("$found")
	done
	ssh_frame "mkdir -p ~/$data/gamedata && chmod 777 ~/$data ~/$data/gamedata"
	for folder in "${src[@]}"; do
		name="$(basename "$folder")"
		for want in "${required[@]}"; do
			[[ "$(echo "$name" | tr '[:upper:]' '[:lower:]')" == "$(echo "$want" | tr '[:upper:]' '[:lower:]')" ]] && name="$want"
		done
		echo "copying $name ..."
		rsync -rt --chmod=Du=rwx,Dgo=rwx,Fu=rw,Fgo=rw "$folder/" "$frame_host:$data/gamedata/$name/"
	done
	copy_port_assets
elif ((from_steam)); then
	# Copy on the headset: Steam's install is outside ~/Documents, which is
	# all Lepton can see. Folder names are matched case-insensitively.
	ssh_frame "bash -s" "$data" <<'SH'
set -euo pipefail
data="$1"
steam="$HOME/.local/share/Steam/steamapps"
src="$steam/common/Grand Theft Auto Vice City"
manifest="$steam/appmanifest_12110.acf"
[ -f "$manifest" ] || { echo "Grand Theft Auto: Vice City (app 12110) is not installed on the Frame" >&2; exit 1; }
if [ -d "$steam/downloading/12110" ] || ! grep -q '"StateFlags"[[:space:]]*"4"' "$manifest"; then
	echo "Steam has not finished installing Vice City yet; wait for the download to complete" >&2
	exit 1
fi
mkdir -p "$HOME/$data/gamedata"
chmod 777 "$HOME/$data" "$HOME/$data/gamedata"
for want in data TEXT anim txd skins mp3 movies models Audio; do
	found="$(find "$src" -maxdepth 1 -type d -iname "$want" | head -1)"
	[ -n "$found" ] || { echo "$src has no $want folder" >&2; exit 1; }
	echo "copying $want ..."
	rsync -rt --chmod=Du=rwx,Dgo=rwx,Fu=rw,Fgo=rw "$found/" "$HOME/$data/gamedata/$want/"
done
SH
	copy_port_assets
elif ((port_assets)); then
	ssh_frame "mkdir -p ~/$data/gamedata && chmod 777 ~/$data ~/$data/gamedata"
	copy_port_assets
fi

# ---- Unattended-test switches (vr-diag.ini in the data folder) ---------------
if ((diag_reset)); then
	ssh_frame "rm -f ~/$data/vr-diag.ini"
	echo "vr-diag.ini removed"
fi
if ((${#diag[@]})); then
	ssh_frame "mkdir -p ~/$data && python3 - ~/$data/vr-diag.ini ${diag[*]}" <<'PY'
import os, sys
path, pairs = sys.argv[1], sys.argv[2:]
lines = open(path).read().splitlines() if os.path.exists(path) else []
for pair in pairs:
    key, _, val = pair.partition("=")
    for i, line in enumerate(lines):
        if line.split("=")[0].strip() == key:
            lines[i] = "%s=%s" % (key, val)
            break
    else:
        lines.append("%s=%s" % (key, val))
open(path, "w").write("\n".join(lines) + "\n")
os.chmod(path, 0o666)
print("vr-diag.ini:", " ".join(lines))
PY
fi
if ((clear_dumps)); then
	ssh_frame "rm -rf ~/$data/vr-dumps"
fi

if ((install || register)); then
	[[ -f "$apk" ]] || die "no APK at $apk (build it with tools/steam_frame/build.sh)"
fi

# ---- Persistent Steam title --------------------------------------------------
title_exists=0
ssh_frame "test -d ~/devkit-game/$title" 2>/dev/null && title_exists=1
if ((register || (title_exists && install))); then
	ssh_frame "mkdir -p ~/devkit-game/$title"
	scp -q "$apk" "$frame_host:devkit-game/$title/$title_apk"
	echo "copied the APK into ~/devkit-game/$title/$title_apk"
fi
if ((register)); then
	parms="{\"gameid\": \"$title\", \"directory\": \"/home/steamos/devkit-game/$title\", \"force_appid\": \"\", \"argv\": [\"$title_apk\"], \"env\": {}, \"settings\": {\"steam_play\": \"1\", \"compat_tool\": \"lepton\"}, \"lepton_args\": \"\"}"
	# needs the Steam client running on the Frame; it answers with success or an error
	ssh_frame "cd ~/devkit-utils && python3 steam-client-create-shortcut --parms '$parms'"
fi

((use_adb)) || exit 0
[[ -n "$adb_bin" && -x "$adb_bin" ]] || die "adb not found; set ADB"

# ---- Lepton Development over network adb --------------------------------------
"$adb_bin" start-server >/dev/null
for attempt in 1 2 3 4 5; do
	"$adb_bin" connect "$adb_host:5555" >/dev/null 2>&1 || true
	if "$adb_bin" -s "$adb_host:5555" shell true 2>/dev/null; then break; fi
	sleep $((attempt * 2))
	if ((attempt == 5)); then
		die 'cannot reach Lepton Development at '"$adb_host"':5555. Start "Lepton Development" from the Frame'"'"'s Steam library.'
	fi
done
device="$adb_host:5555"
adb() { "$adb_bin" -s "$device" "$@"; }

if ((install)); then
	adb install -r -g "$apk" | grep -i -E 'success|fail|error' || true
	# /sdcard/Documents is a plain symlink to the host's ~/Documents in Lepton
	# 2.8.14, so this grant is not needed there; it is set in case a later
	# Lepton enforces Android's storage rules.
	adb shell appops set "$package" MANAGE_EXTERNAL_STORAGE allow >/dev/null 2>&1 || true
fi

if ((launch)); then
	adb shell am force-stop "$package"
	adb logcat -c
	adb shell am start -n "$package/$activity" >/dev/null
	echo "started $package; follow it with: $adb_bin -s $device logcat -s MiamiVR MiamiVR-game librw-vk"
fi

if ((logcat_seconds > 0)); then
	out="$kit/scratchpad/frame-logcat.txt"
	mkdir -p "$(dirname "$out")"
	("$adb_bin" -s "$device" logcat -v time >"$out" & pid=$!; sleep "$logcat_seconds"; kill "$pid") 2>/dev/null || true
	echo "log: $out"
	grep -E "Steam Frame\) starting|storage root|frame: |OpenXR|xrCreateInstance|vulkan|Vulkan|swapchain|Swapchain|session state|\[vr\]|\[vr-perf\]|\[vr-dump\]|input:|FATAL|crash|abort" "$out" | head -80 || true
fi

if [[ -n "$pull_dumps" ]]; then
	mkdir -p "$pull_dumps"
	scp -q -r "$frame_host:$data/vr-dumps/." "$pull_dumps/"
	if command -v sips >/dev/null; then
		for bmp in "$pull_dumps"/*.bmp; do
			[[ -f "$bmp" ]] && sips -s format png "$bmp" --out "${bmp%.bmp}.png" >/dev/null
		done
	fi
	echo "dumps in $pull_dumps: $(ls "$pull_dumps" | tr '\n' ' ')"
fi
