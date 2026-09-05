#!/usr/bin/env bash
# B-CONFLICT: 3 OS processes. A–B stay in call; C's inbound is rejected; after A
# leaves, C connects (end-and-accept). Direct path, no hop.
# See docs/ops/TEST_STRATEGY.md
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PROBE_BIN="${PP_CALL_PROBE_BIN:-${ROOT}/build/src/app/node/pp-call-probe}"
READY_FILE="${PP_CALL_CONFLICT_READY_FILE:-/tmp/pp-call-conflict.ready}"
LISTEN="${PP_CALL_CONFLICT_LISTEN:-/ip4/127.0.0.1/udp/47120/adp/1.0.0}"

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

rm -f "${READY_FILE}"
"${PROBE_BIN}" --role answerer --listen "${LISTEN}" --ready-file "${READY_FILE}" \
  --hold-seconds 25 --no-auto-detach --call-id pp-call-conflict &
ans_pid=$!
a_pid=""
cleanup() {
  kill "${a_pid}" 2>/dev/null || true
  kill "${ans_pid}" 2>/dev/null || true
  wait "${a_pid}" 2>/dev/null || true
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
echo "B-CONFLICT peer=${peer}"

"${PROBE_BIN}" --role offerer --peer "${peer}" --call-id pp-call-conflict --hold-ms 7000 &
a_pid=$!
sleep 1

echo "=== C tries inbound while A–B active (expect busy) ==="
"${PROBE_BIN}" --role offerer --peer "${peer}" --call-id pp-call-conflict-cb \
  --expect busy --timeout-ms 2500

echo "=== wait A leave ==="
wait "${a_pid}"
a_pid=""
sleep 0.5

echo "=== C end-and-accept after A left ==="
"${PROBE_BIN}" --role offerer --peer "${peer}" --call-id pp-call-conflict
echo "pp-call-conflict smoke PASSED"
