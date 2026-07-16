#!/usr/bin/env bash
# Guard base-layer include edges. See src/base/README.md.
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

check_absent "data must not include ai/" '#include "base/ai/' src/base/data
check_absent "crypto must not include messaging/ThreadTypes.h" \
  '#include "base/messaging/ThreadTypes.h"' src/base/crypto
check_absent "messaging must not include ai/conversation/ConversationTypes.h" \
  '#include "base/ai/conversation/ConversationTypes.h"' src/base/messaging
check_absent "platform headers must not include ui/" \
  '#include "base/ui/' src/base/platform

if [[ "$FAIL" -ne 0 ]]; then
  exit 1
fi

echo "OK: base include edges"
