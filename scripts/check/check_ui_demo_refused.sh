#!/usr/bin/env bash
# The UI demo app (-DPP_BROWSER_UI_DEMO=ON, cmake/UiDemo.cmake) is for the iOS simulator and Android only. Configure
# a host (desktop) build with it, and with a backend address, and check that CMake refuses both. Configure only: no
# compile. Run before pushing (needs cmake; the throwaway build dir lives under the repo's logs/ and is removed).
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK="$ROOT/logs/ui-demo-config-check-$$"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT
FAIL=0

expect_refused() {
  local label="$1" needle="$2"
  shift 2
  local out
  if out="$(cmake -S "$ROOT" -B "$WORK/$label" "$@" 2>&1)"; then
    echo "FAIL: $label: configure succeeded, CMake should refuse it"
    FAIL=1
  elif ! grep -q "$needle" <<<"$out"; then
    echo "FAIL: $label: configure failed, but not with '$needle':"
    echo "$out" | tail -8
    FAIL=1
  else
    echo "ok: $label refused ($needle)"
  fi
}

expect_refused no-dev-tools "needs -DPP_BROWSER_DEV_TOOLS=ON" -DPP_BROWSER_UI_DEMO=ON
expect_refused backend-address "without PP_API_URL_PROD" -DPP_BROWSER_UI_DEMO=ON -DPP_BROWSER_DEV_TOOLS=ON \
  -DPP_API_URL_PROD=https://x.example.test
expect_refused desktop "iOS simulator and Android only" -DPP_BROWSER_UI_DEMO=ON -DPP_BROWSER_DEV_TOOLS=ON

exit "$FAIL"
