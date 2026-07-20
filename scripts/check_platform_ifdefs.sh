#!/usr/bin/env bash
# Fail when OS-specific #ifdefs appear outside allowlisted platform/render paths.
# See docs/architecture/PLATFORM_CODE.md.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FAIL=0

PATTERN='#if(ndef)?[[:space:]]+defined[[:space:]]*\([[:space:]]*(_WIN32|WIN32|__APPLE__|__linux__|__ANDROID__|TARGET_OS_IPHONE)'

check_forbidden() {
  local label="$1"
  local path="$2"
  if rg -q "$PATTERN" "$ROOT/$path" 2>/dev/null; then
    echo "FAIL: $label"
    rg -n "$PATTERN" "$ROOT/$path" || true
    FAIL=1
  fi
}

check_forbidden "feature/ must not use OS #ifdefs" src/feature
check_forbidden "base/data/ must not use OS #ifdefs" src/base/data
check_forbidden "base/net/ must not use OS #ifdefs" src/base/net
check_forbidden "base/ai/ must not use OS #ifdefs" src/base/ai

if rg -q "$PATTERN" "$ROOT/src/app/Application.cpp" 2>/dev/null; then
  echo "FAIL: src/app/Application.cpp must not use OS #ifdefs"
  rg -n "$PATTERN" "$ROOT/src/app/Application.cpp" || true
  FAIL=1
fi

if [[ "$FAIL" -ne 0 ]]; then
  echo ""
  echo "Move OS-specific code to src/base/platform/os/ or src/base/platform/desktop/."
  echo "See docs/architecture/PLATFORM_CODE.md"
  exit 1
fi

echo "OK: platform ifdef locations"
