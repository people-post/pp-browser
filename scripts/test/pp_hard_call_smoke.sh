#!/usr/bin/env bash
# B-HARD-CALL: Invite→InCall→Leave via hop on forced-isolated nets (no A↔B).
# Answerer on peer-b; offerer on peer-a with --via-hop.
#
# Prefer: ./scripts/test/pp_local_test.sh run --suite hard
# See packaging/pp-node/HARD_LAB.md and docs/ops/TEST_STRATEGY.md
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=pp_hard_lab_lib.sh
source "${ROOT}/scripts/test/pp_hard_lab_lib.sh"

CYCLES="${PP_CALL_PROBE_CYCLES:-2}"
CALL_BIN_NAME="pp-call-probe"
SKIP_UP=0
WITH_CHAT=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --status-url) PP_HARD_STATUS_URL="$2"; shift 2 ;;
    --cycles) CYCLES="$2"; shift 2 ;;
    --with-chat) WITH_CHAT=1; shift ;;
    --skip-up) SKIP_UP=1; shift ;;
    -h|--help)
      echo "Usage: $(basename "$0") [--status-url URL] [--cycles K] [--with-chat] [--skip-up]"
      exit 0
      ;;
    *) echo "error: unknown arg: $1" >&2; exit 2 ;;
  esac
done

if [[ ! -x "${PP_HARD_PROBE_DIR}/${CALL_BIN_NAME}" ]]; then
  pp_hard_die "pp-call-probe missing (${PP_HARD_PROBE_DIR}/${CALL_BIN_NAME}); cmake --build build --target pp-call-probe"
fi

pp_hard_ensure_up "${SKIP_UP}"

label="B-HARD-CALL"
call_id="pp-hard-call"
READY_NAME="call.ready"
LISTEN="${PP_HARD_CALL_LISTEN:-/ip4/0.0.0.0/udp/47150/adp/1.0.0}"
if [[ "${WITH_CHAT}" -eq 1 ]]; then
  label="B-HARD-MSG+CALL"
  call_id="pp-hard-call-msg"
  READY_NAME="call-msg.ready"
  LISTEN="${PP_HARD_CALL_MSG_LISTEN:-/ip4/0.0.0.0/udp/47151/adp/1.0.0}"
fi

echo "=== assert isolation still holds ==="
if pp_hard_exec "${PP_HARD_PEER_A}" ping -c1 -W1 "${PEER_B_IP}" >/dev/null 2>&1; then
  pp_hard_die "peer-a unexpectedly reached peer-b (topology not isolated)"
fi
echo "ok  direct A→B blocked"

rm -f "${PP_HARD_SHARE_DIR}/${READY_NAME}"
pp_hard_exec "${PP_HARD_PEER_B}" rm -f "/share/${READY_NAME}"

hold=$((CYCLES * 15 + 40))
ans_args=(/probes/${CALL_BIN_NAME} --role answerer --listen "${LISTEN}"
  --advertise-host "${PEER_B_IP}" --ready-file "/share/${READY_NAME}"
  --hold-seconds "${hold}" --call-id "${call_id}")
if [[ "${WITH_CHAT}" -eq 1 ]]; then
  ans_args+=(--with-chat)
fi

echo "=== ${label} answerer on peer-b ==="
pp_hard_exec "${PP_HARD_PEER_B}" "${ans_args[@]}" &
ans_pid=$!
cleanup() {
  kill "${ans_pid}" 2>/dev/null || true
  wait "${ans_pid}" 2>/dev/null || true
}
trap cleanup EXIT

for _ in $(seq 1 100); do
  if [[ -s "${PP_HARD_SHARE_DIR}/${READY_NAME}" ]]; then
    break
  fi
  sleep 0.1
done
[[ -s "${PP_HARD_SHARE_DIR}/${READY_NAME}" ]] || pp_hard_die "answerer ready-file not written"

peer="$(tr -d '\n' < "${PP_HARD_SHARE_DIR}/${READY_NAME}")"
echo "${label} hop=${HOP_MA_A} peer=${peer} cycles=${CYCLES}"

off_args=(/probes/${CALL_BIN_NAME} --role offerer --peer "${peer}" --via-hop "${HOP_MA_A}"
  --cycles "${CYCLES}" --call-id "${call_id}")
if [[ "${WITH_CHAT}" -eq 1 ]]; then
  off_args+=(--with-chat)
fi

pp_hard_exec "${PP_HARD_PEER_A}" "${off_args[@]}"
# Answerer may still be holding; offerer success is the gate.
kill "${ans_pid}" 2>/dev/null || true
wait "${ans_pid}" 2>/dev/null || true
trap - EXIT

echo "${label} smoke PASSED"
