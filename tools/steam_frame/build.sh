#!/usr/bin/env bash
# Build the Steam Frame APK on x86-64 Linux, macOS, or x86-64 Ubuntu in WSL2.
#
#   tools/steam_frame/build.sh [--work DIR] [--quest]
#
#   --work DIR   work directory (default ~/VCVRFrameBuild). It holds the pinned
#                reVC checkout (reVC/) and the assembled tree (tree/).
#   --quest      also build the Quest debug APK from the same tree, to check
#                that the Frame changes leave it building
#
# First run: clones reVC at the tested commit, runs tools/assemble.sh into
# <work>/tree and commits the result as the tree's git baseline, so that
# tools/steam_frame/sync-from-tree.sh can copy later edits back into the kit.
# Later runs build the existing tree as it is; delete <work>/tree to reassemble
# it from the kit (this discards edits made only in the tree).
#
# Needs: git, a JDK 17 or 21 (JAVA_HOME, or Homebrew openjdk@17/@21), and an
# Android SDK (ANDROID_HOME, default ~/Library/Android/sdk on a Mac) with
# platform 35, NDK 27.2.12479018 and CMake 3.22.1:
#   sdkmanager "platforms;android-35" "ndk;27.2.12479018" "cmake;3.22.1"
# The NDK and CMake are x86_64 on a Mac, so Rosetta must be installed.
#
# Output: <work>/tree/android/app/build/outputs/apk/frame/app-frame.apk
set -euo pipefail

kit="$(cd "$(dirname "$0")/../.." && pwd)"
work="$HOME/VCVRFrameBuild"
quest=0
revc_url="https://github.com/mrxenginner/reVC.git"
revc_commit="026cd10f3fdbd92c089830e5067c4457c53c1b51"

while (($#)); do
	case "$1" in
	--work) work="$2"; shift 2 ;;
	--quest) quest=1; shift ;;
	-h|--help) sed -n '2,/^set -euo/p' "$0" | sed '$d'; exit 0 ;;
	*) echo "unknown option $1" >&2; exit 1 ;;
	esac
done

mkdir -p "$work"
work="$(cd "$work" && pwd)"

if [[ -z "${JAVA_HOME:-}" ]]; then
	for jdk in /opt/homebrew/opt/openjdk@21 /opt/homebrew/opt/openjdk@17 /usr/lib/jvm/java-21-openjdk* /usr/lib/jvm/java-17-openjdk*; do
		[[ -x "$jdk/bin/java" ]] && { export JAVA_HOME="$jdk"; break; }
	done
fi
[[ -n "${JAVA_HOME:-}" ]] || { echo "set JAVA_HOME to a JDK 17 or 21" >&2; exit 1; }
if [[ -z "${ANDROID_HOME:-}" ]]; then
	for sdk in "$HOME/Library/Android/sdk" "$HOME/Android/Sdk"; do
		[[ -d "$sdk" ]] && { export ANDROID_HOME="$sdk"; break; }
	done
fi
[[ -d "${ANDROID_HOME:-}" ]] || { echo "set ANDROID_HOME to an Android SDK" >&2; exit 1; }
[[ -d "$ANDROID_HOME/ndk/27.2.12479018" ]] \
	|| { echo "NDK 27.2.12479018 missing: sdkmanager \"ndk;27.2.12479018\"" >&2; exit 1; }

if [[ ! -f "$work/reVC/src/core/main.cpp" ]]; then
	echo "Fetching reVC $revc_commit ..."
	git clone --no-checkout -b miami "$revc_url" "$work/reVC"
	git -C "$work/reVC" checkout --detach "$revc_commit"
fi
[[ "$(git -C "$work/reVC" rev-parse HEAD)" == "$revc_commit" ]] \
	|| { echo "$work/reVC is not at $revc_commit" >&2; exit 1; }

tree="$work/tree"
if [[ ! -d "$tree" ]]; then
	"$kit/tools/assemble.sh" "$work/reVC" "$tree"
	chmod +x "$tree/android/gradlew"
	(
		cd "$tree"
		printf '%s\n' android/local.properties android/.gradle/ android/app/build/ \
			android/app/.cxx/ third_party/openxr-khronos-*/ > .gitignore
		git init -q
		git add -A
		git -c user.name=build -c user.email=build@localhost commit -qm \
			"Assembled baseline (kit $(git -C "$kit" rev-parse --short HEAD 2>/dev/null || echo unknown), reVC ${revc_commit:0:8})"
	)
	echo "Assembled $tree"
fi

echo "sdk.dir=$ANDROID_HOME" > "$tree/android/local.properties"
tasks=(:app:assembleFrame)
((quest)) && tasks+=(:app:assembleDebug)
(cd "$tree/android" && ./gradlew "${tasks[@]}" --console=plain)

apk="$tree/android/app/build/outputs/apk/frame/app-frame.apk"
echo ""
echo "Steam Frame APK: $apk"
echo "Install it with: tools/steam_frame/deploy.sh --apk \"$apk\""
