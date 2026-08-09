#!/usr/bin/env bash
# Guard feature-layer include edges. See src/feature/README.md.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FAIL=0

check_absent() {
  local label="$1"
  local pattern="$2"
  local path="$3"
  if rg -q "$pattern" "$ROOT/$path" 2>/dev/null; then
    echo "FAIL: $label"
    rg "$pattern" "$ROOT/$path" || true
    FAIL=1
  fi
}

check_absent "feature must not include app/" \
  '#include "app/' src/feature

check_absent "settings must not include messaging/" \
  '#include "feature/messaging/' src/feature/settings
check_absent "settings must not include ui/" \
  '#include "feature/ui/' src/feature/settings
check_absent "settings must not include chat/" \
  '#include "feature/chat/' src/feature/settings
check_absent "settings must not include ai/ (ToolRegistry is in base/ai)" \
  '#include "feature/ai/' src/feature/settings

check_absent "ai must not include messaging/" \
  '#include "feature/messaging/' src/feature/ai
check_absent "ai must not include ui/" \
  '#include "feature/ui/' src/feature/ai
check_absent "ai must not include chat/" \
  '#include "feature/chat/' src/feature/ai

check_absent "messaging must not include ui/" \
  '#include "feature/ui/' src/feature/messaging
check_absent "messaging must not include chat/" \
  '#include "feature/chat/' src/feature/messaging

check_absent "ui must not include chat/" \
  '#include "feature/chat/' src/feature/ui

if [[ "$FAIL" -ne 0 ]]; then
  exit 1
fi

echo "OK: feature include edges"
