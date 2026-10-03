#!/usr/bin/env bash
# Copy Steam Frame work from an assembled build tree back into this kit.
#
#   tools/steam_frame/sync-from-tree.sh <assembled-tree> [<baseline-commit>]
#
# The Frame port is developed inside a tree made by tools/steam_frame/build.sh
# (tools/assemble.sh plus a local git history whose first commit is the
# unmodified assembly). Every file changed or added since that baseline is
# copied back to where assemble.sh takes it from:
#
#   vendor/librw/<path>   -> librw/<path>
#   <path> in overlay/    -> overlay/<path>
#   a new file            -> overlay/<path>
#
# A changed reVC file that the overlay does not carry would need a patch, so
# the script stops and names it instead of copying reVC source into the kit.
# Build products and local files (android/app/build, .cxx, local.properties,
# third_party downloads) are skipped.
set -euo pipefail

kit="$(cd "$(dirname "$0")/../.." && pwd)"
tree="${1:?usage: $0 <assembled-tree> [<baseline-commit>]}"
tree="$(cd "$tree" && pwd)"
git -C "$tree" rev-parse --git-dir >/dev/null 2>&1 \
	|| { echo "$tree is not a git repository (make it with tools/steam_frame/build.sh)" >&2; exit 1; }
baseline="${2:-$(git -C "$tree" rev-list --max-parents=0 HEAD | tail -1)}"

skip() {
	case "$1" in
	android/app/build/*|android/app/.cxx/*|android/.gradle/*|android/build/*) return 0 ;;
	android/local.properties|.gitignore) return 0 ;;
	*.o|*.so|*.apk|.DS_Store|*/.DS_Store) return 0 ;;
	esac
	return 1
}

copied=0
blocked=()
while IFS= read -r -d '' entry; do
	status="${entry%%$'\t'*}"
	path="${entry#*$'\t'}"
	skip "$path" && continue
	case "$path" in
	vendor/librw/*) dest="$kit/librw/${path#vendor/librw/}" ;;
	*)
		dest="$kit/overlay/$path"
		if [[ "$status" != A && ! -e "$dest" ]]; then
			blocked+=("$path")
			continue
		fi
		;;
	esac
	if [[ "$status" == D ]]; then
		echo "deleted in the tree, left alone in the kit: $path" >&2
		continue
	fi
	mkdir -p "$(dirname "$dest")"
	if ! cmp -s "$tree/$path" "$dest"; then
		cp -p "$tree/$path" "$dest"
		echo "  ${dest#$kit/}"
		copied=$((copied + 1))
	fi
done < <(
	# committed changes since the baseline, then uncommitted and untracked ones
	{
		git -C "$tree" diff --name-status --no-renames "$baseline" HEAD
		git -C "$tree" diff --name-status --no-renames HEAD
		git -C "$tree" ls-files --others --exclude-standard | sed 's/^/A\t/'
	} | sort -u -t$'\t' -k2,2 | tr '\n' '\0'
)

echo "copied $copied file(s) into the kit"
if ((${#blocked[@]})); then
	echo "these changed reVC files are not in overlay/ and need a patch in patches/:" >&2
	printf '  %s\n' "${blocked[@]}" >&2
	exit 1
fi
