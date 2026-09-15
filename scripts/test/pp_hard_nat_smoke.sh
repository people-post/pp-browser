#!/usr/bin/env bash
# Wave 5 CGNAT-ish: dual SNAT peers + public hop.
#
# N-HARD-CGNAT-ISH  — topology asserts (A↛B, hop↛peer-private, peers→hop via SNAT)
# B-HARD-CALL-NAT   — 1:1 call via nested circuit (--via-hop --peer-id-only,
#                     answerer --warm-hop --min-rx-frames)
#
# Reproduce gate: if the call fails under this topo, that is the dogfood signal.
#   PP_HARD_NAT_CALL_EXPECT=success  (default) — call must pass (regression wall)
#   PP_HARD_NAT_CALL_EXPECT=fail     — call must fail (lab reproduces the bug)
#
# Prefer: ./scripts/test/pp_local_test.sh run --suite hard-w5
# See packaging/pp-node/HARD_LAB.md Wave 5
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=pp_hard_lab_lib.sh
source "${ROOT}/scripts/test/pp_hard_lab_lib.sh"

CALL_BIN_NAME="pp-call-probe"
SKIP_UP=0
CYCLES="${PP_CALL_PROBE_CYCLES:-1}"
# success = green path required; fail = expect reproduce (call fails)
CALL_EXPECT="${PP_HARD_NAT_CALL_EXPECT:-success}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --status-url) PP_HARD_CGNAT_STATUS_URL="$2"; shift 2 ;;
    --cycles) CYCLES="$2"; shift 2 ;;
    --expect-call) CALL_EXPECT="$2"; shift 2 ;;
    --skip-up) SKIP_UP=1; shift ;;
    -h|--help)
      cat <<EOF
Usage: $(basename "$0") [--status-url URL] [--cycles K] [--expect-call success|fail] [--skip-up]

  N-HARD-CGNAT-ISH + B-HARD-CALL-NAT on dual-SNAT compose.
  --expect-call fail  pass only if the call fails (reproduce dogfood)
  --expect-call success  pass only if Invite→audio RX→Leave works (default)
EOF
      exit 0
      ;;
    *) echo "error: unknown arg: $1" >&2; exit 2 ;;
  esac
done

case "${CALL_EXPECT}" in
  success|fail) ;;
  *) pp_hard_die "--expect-call must be success|fail (got ${CALL_EXPECT})" ;;
esac

if [[ ! -x "${PP_HARD_PROBE_DIR}/${CALL_BIN_NAME}" ]]; then
  pp_hard_die "pp-call-probe missing (${PP_HARD_PROBE_DIR}/${CALL_BIN_NAME}); cmake --build build --target pp-call-probe"
fi

pp_hard_cgnat_ensure_up "${SKIP_UP}"
pp_hard_cgnat_assert_nat_shape

# Peer image has no pkill; clear leftover probes that hold UDP listen ports.
pp_hard_exec "${PP_HARD_CGNAT_PEER_A}" sh -c 'for p in $(pidof pp-call-probe 2>/dev/null); do kill -9 "$p" 2>/dev/null || true; done' || true
pp_hard_exec "${PP_HARD_CGNAT_PEER_B}" sh -c 'for p in $(pidof pp-call-probe 2>/dev/null); do kill -9 "$p" 2>/dev/null || true; done' || true


label="B-HARD-CALL-NAT"
call_id="pp-hard-call-nat"
READY_NAME="call-nat.ready"
LISTEN="${PP_HARD_NAT_CALL_LISTEN:-/ip4/0.0.0.0/udp/47160/adp/1.0.0}"

echo "=== ${label} (expect=${CALL_EXPECT}) answerer on peer-b with --warm-hop ==="
rm -f "${PP_HARD_CGNAT_SHARE_DIR}/${READY_NAME}"
pp_hard_exec "${PP_HARD_CGNAT_PEER_B}" rm -f "/share/${READY_NAME}"

hold=$((CYCLES * 20 + 50))
ans_args=(/probes/${CALL_BIN_NAME} --role answerer --listen "${LISTEN}"
  --advertise-host "${PEER_B_IP}" --ready-file "/share/${READY_NAME}"
  --hold-seconds "${hold}" --call-id "${call_id}"
  --warm-hop "${HOP_MA_PUBLIC}" --min-rx-frames "${CYCLES}")

pp_hard_exec "${PP_HARD_CGNAT_PEER_B}" "${ans_args[@]}" &
ans_pid=$!
cleanup() {
  kill "${ans_pid}" 2>/dev/null || true
  wait "${ans_pid}" 2>/dev/null || true
}
trap cleanup EXIT

for _ in $(seq 1 150); do
  if [[ -s "${PP_HARD_CGNAT_SHARE_DIR}/${READY_NAME}" ]]; then
    break
  fi
  sleep 0.1
done
[[ -s "${PP_HARD_CGNAT_SHARE_DIR}/${READY_NAME}" ]] || pp_hard_die "answerer ready-file not written (warm-hop may have failed)"

# Extra settle so hop peer-book ingest completes after warm-hop.
sleep 1

peer="$(tr -d '\n' < "${PP_HARD_CGNAT_SHARE_DIR}/${READY_NAME}")"
echo "${label} hop=${HOP_MA_PUBLIC} peer=${peer} cycles=${CYCLES}"

off_args=(/probes/${CALL_BIN_NAME} --role offerer --peer "${peer}"
  --via-hop "${HOP_MA_PUBLIC}" --peer-id-only
  --cycles "${CYCLES}" --call-id "${call_id}" --timeout-ms 20000)

set +e
pp_hard_exec "${PP_HARD_CGNAT_PEER_A}" "${off_args[@]}"
off_rc=$?
set -e

# Collect answerer exit (may still be holding).
kill "${ans_pid}" 2>/dev/null || true
set +e
wait "${ans_pid}"
ans_rc=$?
set -e
trap - EXIT

echo "offerer_rc=${off_rc} answerer_rc=${ans_rc}"

call_ok=0
# Offerer exit 0 is the primary gate. Answerer may still be holding and get SIGTERM (143)
# from the driver; prefer clean answerer 0 (min-rx early exit) when available.
if [[ "${off_rc}" -eq 0 ]]; then
  if [[ "${ans_rc}" -eq 0 || "${ans_rc}" -eq 143 || "${ans_rc}" -eq 137 ]]; then
    call_ok=1
  fi
fi

if [[ "${CALL_EXPECT}" == "fail" ]]; then
  if [[ "${call_ok}" -eq 1 ]]; then
    pp_hard_die "${label}: expected REPRODUCE (call fail) but call succeeded — flip PP_HARD_NAT_CALL_EXPECT=success?"
  fi
  echo "ok  REPRODUCED: dual-NAT call failed under CGNAT-ish topo (dogfood signal)"
  echo "N-HARD-CGNAT-ISH + ${label} smoke PASSED (expect=fail / reproduced)"
  exit 0
fi

if [[ "${call_ok}" -ne 1 ]]; then
  echo "error: ${label} call failed under dual-NAT (offerer_rc=${off_rc} answerer_rc=${ans_rc})" >&2
  echo "hint: this may reproduce the dogfood bug; re-run with --expect-call fail to lock reproduce mode" >&2
  exit 1
fi

echo "ok  dual-NAT call Invite→RX→Leave via hop (circuit / peer-id-only)"
echo "N-HARD-CGNAT-ISH + ${label} smoke PASSED"
