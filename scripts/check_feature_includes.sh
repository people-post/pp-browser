#!/usr/bin/env bash
# Guard feature-layer include edges. See src/feature/README.md.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
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

check_absent "settings must not include messaging/" \
  '#include "feature/messaging/' src/feature/settings
check_absent "settings must not include retired feature/ui/" \
  '#include "feature/ui/' src/feature/settings
check_absent "settings must not include gui/" \
  '#include "gui/' src/feature/settings

check_absent "ai must not include messaging/" \
  '#include "feature/messaging/' src/feature/ai
check_absent "ai must not include retired feature/ui/" \
  '#include "feature/ui/' src/feature/ai
check_absent "ai must not include gui/" \
  '#include "gui/' src/feature/ai

check_absent "messaging must not include feature/ai/" \
  '#include "feature/ai/' src/feature/messaging
check_absent "messaging must not include retired feature/ui/" \
  '#include "feature/ui/' src/feature/messaging
check_absent "messaging must not include gui/" \
  '#include "gui/' src/feature/messaging

# Retired top-level feature/chat (F007) and feature/ui (F008) — must stay gone.
if [[ -e "$ROOT/src/feature/chat" ]]; then
  echo "FAIL: top-level src/feature/chat must stay removed (absorbed into gui/chat/)"
  FAIL=1
fi
if [[ -e "$ROOT/src/feature/ui" ]]; then
  echo "FAIL: top-level src/feature/ui must stay removed (lifted to src/gui/)"
  FAIL=1
fi
check_absent "must not include retired feature/chat/ path" \
  '#include "feature/chat/' src
check_absent "must not include retired feature/ui/ path" \
  '#include "feature/ui/' src

if [[ "$FAIL" -ne 0 ]]; then
  exit 1
fi

echo "OK: feature include edges"
