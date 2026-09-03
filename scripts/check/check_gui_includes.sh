#!/usr/bin/env bash
# Guard gui-layer include edges (F008). See src/gui/README.md.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FAIL=0

check_absent() {
  local label="$1"
  local pattern="$2"
  local path="$3"
  if rg -q --glob '*.{h,hpp,c,cc,cpp,cxx}' "$pattern" "$ROOT/$path" 2>/dev/null; then
    echo "FAIL: $label"
    rg --glob '*.{h,hpp,c,cc,cpp,cxx}' "$pattern" "$ROOT/$path" || true
    FAIL=1
  fi
}

if [[ ! -d "$ROOT/src/gui" ]]; then
  echo "FAIL: src/gui missing"
  exit 1
fi

check_absent "gui must not include app/" \
  '#include "app/' src/gui

# gui may include feature/, domain/, foundation/, common/.

if [[ "$FAIL" -ne 0 ]]; then
  exit 1
fi

echo "OK: gui include edges"
