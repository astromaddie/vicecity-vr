#!/usr/bin/env bash
# Host test for the Steam-in-place game data resolver.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$here/../../.."
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
# Same names the real build lists from gamefiles/, plus the one the test uses.
printf '\t"TEXT/american.gxt",\n\t"models/menu.txd",\n\t"models/vrhands/BigHandLeft.uxrh",\n' > "$tmp/gamefiles_manifest.inc"
c++ -std=c++17 -Wall -DGAMEDATA_HOST_TEST -I"$tmp" -I"$repo/src/skel/android" \
	"$repo/src/skel/android/GameData.cpp" "$here/gamedata_test.cpp" -o "$tmp/gamedata_test"
"$tmp/gamedata_test"
