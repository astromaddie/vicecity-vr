#!/bin/sh
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
test_source="$repo/tools/steam_frame/tests/cutscene_skip_hold_test.cpp"
out="${TMPDIR:-/tmp}/gta3-vr-cutscene-skip-test-$$"
trap 'rm -f "$out"' EXIT HUP INT TERM

# The durable native source patch must stop offering a skip once the loaded
# flyby camera has reached its endpoint after FinishCutscene.
grep -Fq '+	return ms_running && ms_loaded && bCamLoaded && !HasCutsceneFinished() &&' \
	"$repo/patches/revc-steam-frame-cutscene-skip.patch"

${CXX:-c++} -std=c++11 -Wall -Wextra -Werror \
	-I"$repo/overlay/src/skel/android" "$test_source" -o "$out"
"$out"
