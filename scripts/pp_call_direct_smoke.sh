#!/usr/bin/env bash
# B-CALL-DIRECT multi-process thin-client smoke (two OS processes).
# See docs/ops/TEST_STRATEGY.md
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROBE_BIN="${PP_CALL_PROBE_BIN:-${ROOT}/build/src/app/node/pp-call-probe}"
CYCLES="${PP_CALL_PROBE_CYCLES:-3}"
READY_FILE="${PP_CALL_PROBE_READY_FILE:-/tmp/pp-call-probe.ready}"
LISTEN="${PP_CALL_PROBE_LISTEN:-/ip4/127.0.0.1/tcp/47100}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --probe-bin) PROBE_BIN="$2"; shift 2 ;;
    --cycles) CYCLES="$2"; shift 2 ;;
    -h|--help)
      echo "Usage: $(basename "$0") [--probe-bin PATH] [--cycles K]"
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

rm -f "${READY_FILE}"
hold=$((CYCLES * 5 + 15))
"${PROBE_BIN}" --role answerer --listen "${LISTEN}" --ready-file "${READY_FILE}" \
  --hold-seconds "${hold}" &
ans_pid=$!
cleanup() {
  kill "${ans_pid}" 2>/dev/null || true
  wait "${ans_pid}" 2>/dev/null || true
  rm -f "${READY_FILE}"
}
trap cleanup EXIT

for _ in $(seq 1 50); do
  if [[ -s "${READY_FILE}" ]]; then
    break
  fi
  sleep 0.1
done
if [[ ! -s "${READY_FILE}" ]]; then
  echo "error: answerer ready-file not written" >&2
  exit 1
fi
peer="$(tr -d '\n' < "${READY_FILE}")"
echo "B-CALL-DIRECT peer=${peer} cycles=${CYCLES}"
"${PROBE_BIN}" --role offerer --peer "${peer}" --cycles "${CYCLES}"
echo "pp-call-direct smoke PASSED"
