#!/usr/bin/env bash
# pp-node closure report (info only — not a gate).
#
# Which repo files end up in the pp-node binary, and which of them changed since the last node
# release. Derived from the build graph (ninja -t inputs + recorded header deps), so there is no
# hand-kept path list to rot: move a file, and the next report follows it.
#
# Usage: scripts/dev/pp_node_closure.sh [--build DIR] [--since REF] [--list] [--target NAME]
#   --build DIR    Ninja build dir with pp-node built (default: build). Header deps are only
#                  recorded for objects that compiled, so build pp-node first.
#   --since REF    Diff base (default: newest pp-node/v* tag).
#   --list         Print every closure file, not just the per-module counts.
#   --target NAME  Ninja target (default: src/app/node/pp-node).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="$ROOT/build"
SINCE=""
LIST=0
TARGET="src/app/node/pp-node"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build) BUILD="$(cd "$2" && pwd)"; shift 2 ;;
    --since) SINCE="$2"; shift 2 ;;
    --list) LIST=1; shift ;;
    --target) TARGET="$2"; shift 2 ;;
    -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

[[ -f "$BUILD/build.ninja" ]] || { echo "no build.ninja in $BUILD" >&2; exit 2; }
if [[ -z "$SINCE" ]]; then
  SINCE="$(git -C "$ROOT" tag -l 'pp-node/v*' --sort=-v:refname | head -1)"
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# Sources + objects feeding the binary (recursive through static libs).
ninja -C "$BUILD" -t inputs "$TARGET" > "$tmp/inputs"
grep -E '\.o(bj)?$' "$tmp/inputs" > "$tmp/objects" || true
# Headers each object compiled against (recorded by the last build).
: > "$tmp/deps"
if [[ -s "$tmp/objects" ]]; then
  (cd "$BUILD" && xargs ninja -t deps < "$tmp/objects") > "$tmp/deps"
fi
missing="$(grep -c 'deps.*(STALE)\|#deps 0' "$tmp/deps" || true)"

# Repo-relative files only: drop the build dir (FetchContent sources are pins, reported below)
# and anything outside the repo (system headers).
{
  grep -vE '\.o(bj)?$' "$tmp/inputs"
  grep -E '^    ' "$tmp/deps" | sed 's/^ *//'
} | while IFS= read -r f; do
  case "$f" in
    /*) ;;
    *) f="$BUILD/$f" ;;
  esac
  case "$f" in
    "$BUILD"/*) ;;
    "$ROOT"/*) echo "${f#"$ROOT"/}" ;;
  esac
done | sort -u > "$tmp/closure"

module_of() {
  # src/<layer>/<module>, third_party/<lib>, else top dir.
  awk -F/ '{
    if ($1 == "src" && NF > 3) print $1 "/" $2 "/" $3;
    else if ($1 == "third_party" && NF > 2) print $1 "/" $2;
    else if (NF > 1) print $1 "/" $2;
    else print $1
  }'
}

echo "pp-node closure ($TARGET, build: ${BUILD#"$ROOT"/})"
echo "  files: $(wc -l < "$tmp/closure")  objects: $(wc -l < "$tmp/objects")"
if [[ "${missing:-0}" != "0" ]]; then
  echo "  note: $missing objects have stale/empty header deps — rebuild pp-node for a full report"
fi
echo
echo "== Files per module"
module_of < "$tmp/closure" | sort | uniq -c | sort -k2

if [[ "$LIST" -eq 1 ]]; then
  echo
  echo "== Closure files"
  cat "$tmp/closure"
fi

if [[ -z "$SINCE" ]]; then
  echo
  echo "(no pp-node/v* tag found; pass --since REF for a change report)"
  exit 0
fi

echo
echo "== Closure files changed since $SINCE"
git -C "$ROOT" diff --name-status "$SINCE"...HEAD -- $(cat "$tmp/closure") | sort -k2 > "$tmp/changed" || true
if [[ -s "$tmp/changed" ]]; then
  cat "$tmp/changed"
  echo
  echo "== Changed files per module"
  awk '{print $NF}' "$tmp/changed" | module_of | sort | uniq -c | sort -k2
else
  echo "  (none)"
fi

# New files entering the closure show up as A above only when added in-range; a file that
# already existed but newly joined the closure is not visible from a single build.

echo
echo "== Sibling pins since $SINCE"
pins_changed=0
for f in "$ROOT"/cmake/PpCpp*.cmake; do
  rel="${f#"$ROOT"/}"
  # PpCppAmp.cmake -> _deps/pp_cpp_amp-*: only pins whose sources feed this binary.
  dep="$(basename "$f" .cmake | sed -E 's/([a-z])([A-Z])/\1_\2/g' | tr 'A-Z' 'a-z')"
  if [[ -d "$BUILD/_deps/$dep-src" ]] && ! grep -q "/_deps/$dep-" "$tmp/inputs"; then
    continue
  fi
  if ! git -C "$ROOT" diff --quiet "$SINCE"...HEAD -- "$rel"; then
    old="$(git -C "$ROOT" show "$SINCE:$rel" 2>/dev/null | grep -oE '_GIT_TAG "[^"]+"' | head -1 || true)"
    new="$(grep -oE '_GIT_TAG "[^"]+"' "$f" | head -1 || true)"
    echo "  $rel: ${old#_GIT_TAG } -> ${new#_GIT_TAG }"
    pins_changed=1
  fi
done
[[ "$pins_changed" -eq 1 ]] || echo "  (none)"
