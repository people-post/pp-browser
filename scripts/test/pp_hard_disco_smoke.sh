#!/usr/bin/env bash
# Wave 3 hard-lab discovery: N-HARD-STALE-ADDR / N-HARD-SEED-ONLY.
#
# Prefer: ./scripts/test/pp_local_test.sh run --suite hard-w3
# See packaging/pp-node/HARD_LAB.md and docs/ops/TEST_STRATEGY.md
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=pp_hard_lab_lib.sh
source "${ROOT}/scripts/test/pp_hard_lab_lib.sh"

PROFILE=""
SKIP_UP=0
PROBE_BIN_NAME="pp-node-probe"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --profile) PROFILE="$2"; shift 2 ;;
    --status-url) PP_HARD_STATUS_URL="$2"; shift 2 ;;
    --skip-up) SKIP_UP=1; shift ;;
    -h|--help)
      cat <<EOF
Usage: $(basename "$0") --profile stale-addr|seed-only [--status-url URL] [--skip-up]

  stale-addr  N-HARD-STALE-ADDR — direct stale dial fails; circuit via hop with real MA
  seed-only   N-HARD-SEED-ONLY  — PeerId-only StartBridge after target warms hop
EOF
      exit 0
      ;;
    *) echo "error: unknown arg: $1" >&2; exit 2 ;;
  esac
done

[[ -n "${PROFILE}" ]] || pp_hard_die "missing --profile stale-addr|seed-only"

if [[ ! -x "${PP_HARD_PROBE_DIR}/${PROBE_BIN_NAME}" ]]; then
  pp_hard_die "probe missing (${PP_HARD_PROBE_DIR}/${PROBE_BIN_NAME}); cmake --build build --target pp-node-probe"
fi

pp_hard_ensure_up "${SKIP_UP}"

echo "=== assert isolation: peer-a cannot reach peer-b ==="
if pp_hard_exec "${PP_HARD_PEER_A}" ping -c1 -W1 "${PEER_B_IP}" >/dev/null 2>&1; then
  pp_hard_die "peer-a unexpectedly reached peer-b at ${PEER_B_IP} (topology not isolated)"
fi
echo "ok  direct A→B blocked"

run_stale_addr() {
  local ready_name="disco-stale.ready"
  echo "=== N-HARD-STALE-ADDR ==="
  rm -f "${PP_HARD_SHARE_DIR}/${ready_name}"
  pp_hard_exec "${PP_HARD_PEER_B}" rm -f "/share/${ready_name}"

  pp_hard_exec "${PP_HARD_PEER_B}" /probes/${PROBE_BIN_NAME} --mode bridge-target \
    --advertise-host "${PEER_B_IP}" --ready-file "/share/${ready_name}" --hold-seconds 60 &
  local tgt_pid=$!
  cleanup_tgt() {
    kill "${tgt_pid}" 2>/dev/null || true
    wait "${tgt_pid}" 2>/dev/null || true
  }
  trap cleanup_tgt EXIT

  for _ in $(seq 1 100); do
    if [[ -s "${PP_HARD_SHARE_DIR}/${ready_name}" ]]; then
      break
    fi
    sleep 0.1
  done
  [[ -s "${PP_HARD_SHARE_DIR}/${ready_name}" ]] || pp_hard_die "bridge-target ready-file not written"

  local target_peer
  target_peer="$(head -n1 "${PP_HARD_SHARE_DIR}/${ready_name}" | tr -d '\n')"
  [[ -n "${target_peer}" ]] || pp_hard_die "empty peer id in ready-file"
  local stale_ma="/ip4/10.255.255.1/udp/9/adp/1.0.0/p2p/${target_peer}"
  echo "stale_ma=${stale_ma}"

  pp_hard_exec "${PP_HARD_PEER_A}" /probes/${PROBE_BIN_NAME} --mode direct-expect-fail \
    --stale-ma "${stale_ma}"
  echo "ok  stale direct failed"

  pp_hard_exec "${PP_HARD_PEER_A}" /probes/${PROBE_BIN_NAME} --mode bridge-via-hop \
    --hop "${HOP_MA_A}" --target-file "/share/${ready_name}"
  wait "${tgt_pid}"
  trap - EXIT
  echo "ok  hop path with real MA"
  echo "N-HARD-STALE-ADDR smoke PASSED"
}

run_seed_only() {
  local ready_name="disco-seed.ready"
  echo "=== N-HARD-SEED-ONLY ==="
  rm -f "${PP_HARD_SHARE_DIR}/${ready_name}"
  pp_hard_exec "${PP_HARD_PEER_B}" rm -f "/share/${ready_name}"

  pp_hard_exec "${PP_HARD_PEER_B}" /probes/${PROBE_BIN_NAME} --mode bridge-target \
    --advertise-host "${PEER_B_IP}" --ready-file "/share/${ready_name}" --hold-seconds 60 \
    --warm-hop "${HOP_MA_B}" &
  local tgt_pid=$!
  cleanup_tgt() {
    kill "${tgt_pid}" 2>/dev/null || true
    wait "${tgt_pid}" 2>/dev/null || true
  }
  trap cleanup_tgt EXIT

  for _ in $(seq 1 150); do
    if [[ -s "${PP_HARD_SHARE_DIR}/${ready_name}" ]]; then
      break
    fi
    sleep 0.1
  done
  [[ -s "${PP_HARD_SHARE_DIR}/${ready_name}" ]] || pp_hard_die "bridge-target ready-file not written"

  # Extra settle so hop peer-book ingest completes after warm-hop associate.
  sleep 1

  pp_hard_exec "${PP_HARD_PEER_A}" /probes/${PROBE_BIN_NAME} --mode bridge-via-hop \
    --hop "${HOP_MA_A}" --target-file "/share/${ready_name}" --peer-id-only
  wait "${tgt_pid}"
  trap - EXIT
  echo "ok  PeerId-only circuit via hop book"
  echo "N-HARD-SEED-ONLY smoke PASSED"
}

case "${PROFILE}" in
  stale-addr) run_stale_addr ;;
  seed-only) run_seed_only ;;
  *) pp_hard_die "unknown --profile ${PROFILE} (stale-addr|seed-only)" ;;
esac
