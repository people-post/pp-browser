#!/usr/bin/env bash
# Fail when OS-specific #ifdefs appear outside allowlisted paths.
# See docs/architecture/PLATFORM_CODE.md.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FAIL=0

if ! command -v rg >/dev/null 2>&1; then
  echo "FAIL: ripgrep (rg) is required for check_platform_ifdefs.sh"
  exit 1
fi

# Matches `#if defined(_WIN32)` and `#ifdef __APPLE__` / `#ifndef TARGET_OS_IPHONE`.
PATTERN_DEFINED='#if(ndef)?[[:space:]]+defined[[:space:]]*\([[:space:]]*(_WIN32|WIN32|__APPLE__|__linux__|__ANDROID__|TARGET_OS_IPHONE)'
PATTERN_IFDEF='#ifn?def[[:space:]]+(_WIN32|WIN32|__APPLE__|__linux__|__ANDROID__|TARGET_OS_IPHONE)'

BACKEND_GLOBS=(
  --glob '!*_Win32.*'
  --glob '!*_Posix.*'
  --glob '!*_Darwin.*'
  --glob '!*_Linux.*'
  --glob '!*_Android.*'
  --glob '!*_Ios.*'
  --glob '!*_Default.*'
)

check_tree() {
  local label="$1"
  local path="$2"
  shift 2
  local extra=("$@")
  if rg -q -e "$PATTERN_DEFINED" -e "$PATTERN_IFDEF" "$ROOT/$path" --glob '!**/tests/**' "${extra[@]}" 2>/dev/null; then
    echo "FAIL: $label"
    rg -n -e "$PATTERN_DEFINED" -e "$PATTERN_IFDEF" "$ROOT/$path" --glob '!**/tests/**' "${extra[@]}" || true
    FAIL=1
  fi
}

check_tree "feature/ must not use OS #ifdefs" src/feature
check_tree "app/ must not use OS #ifdefs" src/app
check_tree "base/data/ must not use OS #ifdefs" src/foundation/data
check_tree "base/net/ must not use OS #ifdefs" src/base/net
check_tree "base/ai/ must not use OS #ifdefs" src/base/ai
check_tree "base/crypto/ must not use OS #ifdefs" src/foundation/crypto
check_tree "base/messaging/ must not use OS #ifdefs" src/base/messaging
check_tree "domain/people/ must not use OS #ifdefs" src/domain/people
check_tree "base/ui/ must not use OS #ifdefs" src/base/ui
check_tree "foundation/error/ must not use OS #ifdefs" src/foundation/error
check_tree "foundation/i18n/ must not use OS #ifdefs" src/foundation/i18n
check_tree "foundation/runtime/ must not use OS #ifdefs" src/foundation/runtime

check_tree "domain/media/ portable TUs must not use OS #ifdefs" src/domain/media \
  "${BACKEND_GLOBS[@]}"
check_tree "base/mesh/ portable TUs must not use OS #ifdefs" src/base/mesh \
  "${BACKEND_GLOBS[@]}"

check_tree "common/ must not use OS #ifdefs (except CivilTime.cpp, WorkerPool.cpp, Logger.h)" src/common \
  --glob '!CivilTime.cpp' --glob '!WorkerPool.cpp' --glob '!Logger.h'

if [[ "$FAIL" -ne 0 ]]; then
  echo ""
  echo "Move OS-specific code to src/foundation/platform/, or a dedicated"
  echo "*_{Win32,Posix,Darwin,Linux,Android,Ios,Default} backend next to the module."
  echo "See docs/architecture/PLATFORM_CODE.md"
  exit 1
fi

echo "OK: platform ifdef locations"
