#!/usr/bin/env bash
# Guard feature-layer include edges. See src/feature/README.md.
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

check_absent "feature must not include app/" \
  '#include "app/' src/feature

check_absent "feature must not include gui/" \
  '#include "gui/' src/feature

check_absent "settings must not include conversations/" \
  '#include "feature/conversations/' src/feature/settings
check_absent "settings must not include retired feature/ui/" \
  '#include "feature/ui/' src/feature/settings
check_absent "settings must not include gui/" \
  '#include "gui/' src/feature/settings

check_absent "ai must not include conversations/" \
  '#include "feature/conversations/' src/feature/ai
check_absent "ai must not include retired feature/ui/" \
  '#include "feature/ui/' src/feature/ai
check_absent "ai must not include gui/" \
  '#include "gui/' src/feature/ai

check_absent "conversations must not include feature/ai/" \
  '#include "feature/ai/' src/feature/conversations
check_absent "conversations must not include retired feature/ui/" \
  '#include "feature/ui/' src/feature/conversations
check_absent "conversations must not include gui/" \
  '#include "gui/' src/feature/conversations

check_absent "calls must not include conversations/" \
  '#include "feature/conversations/' src/feature/calls
check_absent "calls must not include feature/ai/" \
  '#include "feature/ai/' src/feature/calls
check_absent "calls must not include gui/" \
  '#include "gui/' src/feature/calls

# Include-path bans for retired feature folders (chat → gui/chat; ui → gui; messaging → conversations).
check_absent "must not include retired feature/chat/ path" \
  '#include "feature/chat/' src
check_absent "must not include retired feature/ui/ path" \
  '#include "feature/ui/' src
check_absent "must not include retired feature/messaging/ path" \
  '#include "feature/messaging/' src
check_absent "must not include retired nested conversations/calls/ path" \
  '#include "feature/conversations/calls/' src

if [[ "$FAIL" -ne 0 ]]; then
  exit 1
fi

echo "OK: feature include edges"
