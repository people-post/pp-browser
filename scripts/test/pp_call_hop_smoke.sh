#!/usr/bin/env bash
# B-CALL-HOP: two thin clients, call-media hello + audio via packaged circuit hop.
# Answerer listens on 0.0.0.0; hop dials docker0 advertise-host. Offerer does not
# register a direct path to the answerer.
#
# See docs/ops/TEST_STRATEGY.md (B-CALL-HOP)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=pp_node_hop_lib.sh
source "${ROOT}/scripts/test/pp_node_hop_lib.sh"

STATUS_URL="${PP_NODE_STATUS_URL:-http://127.0.0.1:18518}"
PROBE_BIN="${PP_CALL_PROBE_BIN:-${ROOT}/build/src/app/node/pp-call-probe}"
CYCLES="${PP_CALL_PROBE_CYCLES:-2}"
READY_FILE="${PP_CALL_PROBE_READY_FILE:-/tmp/pp-call-hop.ready}"
LISTEN="${PP_CALL_HOP_LISTEN:-/ip4/0.0.0.0/udp/47110/adp/1.0.0}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --status-url) STATUS_URL="$2"; shift 2 ;;
    --probe-bin) PROBE_BIN="$2"; shift 2 ;;
    --cycles) CYCLES="$2"; shift 2 ;;
    -h|--help)
      echo "Usage: $(basename "$0") [--status-url URL] [--probe-bin PATH] [--cycles K]"
      exit 0
      ;;
    *) echo "error: unknown arg: $1" >&2; exit 2 ;;
  esac
done

export PP_NODE_STATUS_URL="${STATUS_URL}"
bash "${ROOT}/scripts/test/pp_node_image_smoke.sh"

if [[ ! -x "${PROBE_BIN}" ]]; then
  echo "error: pp-call-probe missing (${PROBE_BIN}); build with:" >&2
  echo "  cmake --build build --target pp-call-probe" >&2
  exit 1
fi

hop="$(pp_node_hop_multiaddr "${STATUS_URL}")"
advertise_host="$(pp_node_advertise_host)"
if [[ "${advertise_host}" == "127.0.0.1" ]]; then
  echo "warn: using advertise-host=127.0.0.1 (hop-call fails if hop is in Docker)"
else
  echo "B-CALL-HOP advertise-host: ${advertise_host}"
fi

rm -f "${READY_FILE}"
hold=$((CYCLES * 8 + 20))
"${PROBE_BIN}" --role answerer --listen "${LISTEN}" --advertise-host "${advertise_host}" \
  --ready-file "${READY_FILE}" --hold-seconds "${hold}" --call-id pp-call-hop &
ans_pid=$!
cleanup() {
  kill "${ans_pid}" 2>/dev/null || true
  wait "${ans_pid}" 2>/dev/null || true
  rm -f "${READY_FILE}"
}
trap cleanup EXIT

for _ in $(seq 1 80); do
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
echo "B-CALL-HOP hop=${hop} peer=${peer} cycles=${CYCLES}"
"${PROBE_BIN}" --role offerer --peer "${peer}" --via-hop "${hop}" --cycles "${CYCLES}" \
  --call-id pp-call-hop
echo "pp-call-hop smoke PASSED"
