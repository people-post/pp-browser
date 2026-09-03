#!/usr/bin/env bash
# N-HARD-FORCE: isolated net_a / net_b peers; circuit + media via dual-homed hop.
#
# Prefer: ./scripts/pp_local_test.sh run --suite hard
# See packaging/pp-node/HARD_LAB.md and docs/ops/TEST_STRATEGY.md
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=pp_hard_lab_lib.sh
source "${ROOT}/scripts/pp_hard_lab_lib.sh"

PROBE_BIN_NAME="pp-node-probe"
SKIP_UP=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --status-url) PP_HARD_STATUS_URL="$2"; shift 2 ;;
    --skip-up) SKIP_UP=1; shift ;;
    -h|--help)
      echo "Usage: $(basename "$0") [--status-url URL] [--skip-up]"
      exit 0
      ;;
    *) echo "error: unknown arg: $1" >&2; exit 2 ;;
  esac
done

if [[ ! -x "${PP_HARD_PROBE_DIR}/${PROBE_BIN_NAME}" ]]; then
  pp_hard_die "probe missing (${PP_HARD_PROBE_DIR}/${PROBE_BIN_NAME}); cmake --build build --target pp-node-probe"
fi

pp_hard_ensure_up "${SKIP_UP}"

echo "=== assert isolation: peer-a cannot reach peer-b ==="
if pp_hard_exec "${PP_HARD_PEER_A}" ping -c1 -W1 "${PEER_B_IP}" >/dev/null 2>&1; then
  pp_hard_die "peer-a unexpectedly reached peer-b at ${PEER_B_IP} (topology not isolated)"
fi
echo "ok  direct A→B blocked"

echo "=== assert peers can reach hop ==="
pp_hard_exec "${PP_HARD_PEER_A}" ping -c1 -W2 "${HOP_IP_A}" >/dev/null
pp_hard_exec "${PP_HARD_PEER_B}" ping -c1 -W2 "${HOP_IP_B}" >/dev/null
echo "ok  A→hop and B→hop"

echo "=== circuit via hop (bridge-target on B, bridge-via-hop on A) ==="
rm -f "${PP_HARD_SHARE_DIR}/target.ready"
pp_hard_exec "${PP_HARD_PEER_B}" rm -f /share/target.ready
pp_hard_exec "${PP_HARD_PEER_B}" /probes/${PROBE_BIN_NAME} --mode bridge-target \
  --advertise-host "${PEER_B_IP}" --ready-file /share/target.ready --hold-seconds 40 &
tgt_pid=$!
cleanup_tgt() {
  kill "${tgt_pid}" 2>/dev/null || true
  wait "${tgt_pid}" 2>/dev/null || true
}
trap cleanup_tgt EXIT

for _ in $(seq 1 80); do
  if [[ -s "${PP_HARD_SHARE_DIR}/target.ready" ]]; then
    break
  fi
  sleep 0.1
done
[[ -s "${PP_HARD_SHARE_DIR}/target.ready" ]] || pp_hard_die "bridge-target ready-file not written"

pp_hard_exec "${PP_HARD_PEER_A}" /probes/${PROBE_BIN_NAME} --mode bridge-via-hop \
  --hop "${HOP_MA_A}" --target-file /share/target.ready
wait "${tgt_pid}"
trap - EXIT
echo "ok  circuit payload A→hop→B"

echo "=== media fan-out via hop (recv on B, send on A) ==="
rm -f "${PP_HARD_SHARE_DIR}/media-recv.ready"
pp_hard_exec "${PP_HARD_PEER_B}" rm -f /share/media-recv.ready
pp_hard_exec "${PP_HARD_PEER_B}" /probes/${PROBE_BIN_NAME} --mode media-recv \
  --hop "${HOP_MA_B}" --call-id pp-hard-force --hold-seconds 40 \
  --ready-file /share/media-recv.ready &
recv_pid=$!
cleanup_recv() {
  kill "${recv_pid}" 2>/dev/null || true
  wait "${recv_pid}" 2>/dev/null || true
}
trap cleanup_recv EXIT

for _ in $(seq 1 100); do
  if [[ -s "${PP_HARD_SHARE_DIR}/media-recv.ready" ]]; then
    break
  fi
  sleep 0.1
done
[[ -s "${PP_HARD_SHARE_DIR}/media-recv.ready" ]] || pp_hard_die "media-recv did not attach in time"

pp_hard_exec "${PP_HARD_PEER_A}" /probes/${PROBE_BIN_NAME} --mode media-send \
  --hop "${HOP_MA_A}" --call-id pp-hard-force --hold-seconds 12
wait "${recv_pid}"
trap - EXIT
echo "ok  media frame A→hop→B"

echo "N-HARD-FORCE smoke PASSED"
