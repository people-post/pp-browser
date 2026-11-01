#!/usr/bin/env bash
# Guard gui-layer include edges (F008). See src/gui/README.md.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=scripts/check/source_trees.sh
source "$ROOT/scripts/check/source_trees.sh"
FAIL=0

check_absent() {
  local label="$1"
  local pattern="$2"
  local path="$3"
  local trees=() tree
  while IFS= read -r tree; do trees+=("$tree"); done < <(src_trees "$path")
  [[ ${#trees[@]} -gt 0 ]] || return 0
  if rg -q --glob '*.{h,hpp,c,cc,cpp,cxx}' "$pattern" "${trees[@]}" 2>/dev/null; then
    echo "FAIL: $label"
    rg --glob '*.{h,hpp,c,cc,cpp,cxx}' "$pattern" "${trees[@]}" || true
    FAIL=1
  fi
}

if [[ ! -d "$ROOT/src/gui" ]]; then
  echo "FAIL: src/gui missing"
  exit 1
fi

check_absent "gui must not include app/" \
  '#include "app/' src/gui

# gui may include feature/, lib/, foundation/, common/.

if [[ "$FAIL" -ne 0 ]]; then
  exit 1
fi

echo "OK: gui include edges"
