#!/usr/bin/env bash
# B-MIX: run existing loopback thin-client smokes in parallel (interference).
# Allowlist: call ∥ conflict ∥ msg-call. Distinct listen ports / ready-files.
# Not same-session mix (that is B-MSG+CALL). See docs/ops/TEST_STRATEGY.md
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=pp_mix_lib.sh
source "${ROOT}/scripts/pp_mix_lib.sh"

PROBE_BIN="${PP_CALL_PROBE_BIN:-${ROOT}/build/src/app/node/pp-call-probe}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --probe-bin) PROBE_BIN="$2"; shift 2 ;;
    -h|--help)
      echo "Usage: $(basename "$0") [--probe-bin PATH]"
      exit 0
      ;;
    *) echo "error: unknown arg: $1" >&2; exit 2 ;;
  esac
done

if [[ ! -x "${PROBE_BIN}" ]]; then
  echo "error: pp-call-probe missing (${PROBE_BIN}); build with:" >&2
  echo "  cmake --build build --target pp-call-probe" >&2
  exit 1
fi

export PP_CALL_PROBE_BIN="${PROBE_BIN}"
echo "=== B-MIX browser interference (call ∥ conflict ∥ msg-call) ==="
pp_mix_run_parallel \
  call "bash '${ROOT}/scripts/pp_call_direct_smoke.sh' --probe-bin '${PROBE_BIN}'" \
  conflict "bash '${ROOT}/scripts/pp_call_conflict_smoke.sh' --probe-bin '${PROBE_BIN}'" \
  msg-call "bash '${ROOT}/scripts/pp_call_msg_smoke.sh' --probe-bin '${PROBE_BIN}'"
echo "pp-mix-browser smoke PASSED"
