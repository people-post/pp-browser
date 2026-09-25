#!/usr/bin/env bash
# Wave 5 CGNAT-ish: dual SNAT peers + public hop.
#
# N-HARD-CGNAT-ISH     — topology asserts (A↛B, hop↛peer-private, peers→hop via SNAT)
# B-HARD-CALL-NAT      — Phase-1: forced nested circuit (--via-hop --peer-id-only)
# B-HARD-CALL-NAT-PRODUCT — Phase-2: product punch→circuit (--reach product --via-hop seed)
# B-HARD-CALL-NAT-DIRTY   — Phase-3: dirty-book Bridge Ensure (--reach bridge --force-dial-fail)
# B-HARD-CALL-NAT-STACK   — Phase-4: Invite/Accept control + dirty-book media (--product-stack)
#
# Reproduce gate (applies to the selected call phase):
#   PP_HARD_NAT_CALL_EXPECT=success  (default) — call must pass
#   PP_HARD_NAT_CALL_EXPECT=fail     — call must fail (lab reproduces dogfood)
#
# Prefer: ./scripts/test/pp_local_test.sh run --suite hard-w5
# See packaging/pp-node/HARD_LAB.md Wave 5 + projects/hard-lab/DECISIONS.md HL004
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=pp_hard_lab_lib.sh
source "${ROOT}/scripts/test/pp_hard_lab_lib.sh"

CALL_BIN_NAME="pp-call-probe"
SKIP_UP=0
CYCLES="${PP_CALL_PROBE_CYCLES:-1}"
CALL_EXPECT="${PP_HARD_NAT_CALL_EXPECT:-success}"
# circuit | product | dirty | stack | both | all
PHASE="${PP_HARD_NAT_PHASE:-all}"
# Stack phase long-hold knobs (one-way stall repro, dogfood 2026-09-24 16:17):
#   PP_HARD_NAT_STACK_HOLD_MS  offerer hold after media (default 3000)
#   PP_HARD_NAT_RX_STALL_MS    both probes fail if rx frames go flat this long (default 0 = off)
STACK_HOLD_MS="${PP_HARD_NAT_STACK_HOLD_MS:-3000}"
RX_STALL_MS="${PP_HARD_NAT_RX_STALL_MS:-0}"
#   PP_HARD_NAT_NETEM_A / _B   tc netem spec on peer-a / peer-b during calls (e.g. cellular-ish
#                              "delay 150ms 40ms distribution normal loss 2%"); empty = clean
NETEM_A="${PP_HARD_NAT_NETEM_A:-}"
NETEM_B="${PP_HARD_NAT_NETEM_B:-}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --status-url) PP_HARD_CGNAT_STATUS_URL="$2"; shift 2 ;;
    --cycles) CYCLES="$2"; shift 2 ;;
    --expect-call) CALL_EXPECT="$2"; shift 2 ;;
    --phase) PHASE="$2"; shift 2 ;;
    --skip-up) SKIP_UP=1; shift ;;
    -h|--help)
      cat <<EOF
Usage: $(basename "$0") [--status-url URL] [--cycles K] [--phase PHASE]
                        [--expect-call success|fail] [--skip-up]

  PHASE: circuit|product|dirty|stack|both|all
    both   = circuit + product (legacy)
    all    = circuit + product + dirty + stack (HL004 default)
  --expect-call fail  pass only if the call fails (reproduce dogfood)
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
case "${PHASE}" in
  circuit|product|dirty|stack|both|all) ;;
  *) pp_hard_die "--phase must be circuit|product|dirty|stack|both|all (got ${PHASE})" ;;
esac

if [[ ! -x "${PP_HARD_PROBE_DIR}/${CALL_BIN_NAME}" ]]; then
  pp_hard_die "pp-call-probe missing (${PP_HARD_PROBE_DIR}/${CALL_BIN_NAME}); cmake --build build --target pp-call-probe"
fi

pp_hard_cgnat_ensure_up "${SKIP_UP}"
pp_hard_cgnat_assert_nat_shape

pp_hard_kill_peer_probes() {
  pp_hard_exec "${PP_HARD_CGNAT_PEER_A}" sh -c 'for p in $(pidof pp-call-probe 2>/dev/null); do kill -9 "$p" 2>/dev/null || true; done' || true
  pp_hard_exec "${PP_HARD_CGNAT_PEER_B}" sh -c 'for p in $(pidof pp-call-probe 2>/dev/null); do kill -9 "$p" 2>/dev/null || true; done' || true
}

# run_nat_call <label> <call_id> <ready_name> <listen_ma> <mode>
# mode: circuit | product | dirty | stack
run_nat_call() {
  local label="$1"
  local call_id="$2"
  local ready_name="$3"
  local listen_ma="$4"
  local mode="$5"

  pp_hard_kill_peer_probes

  echo "=== ${label} (expect=${CALL_EXPECT} mode=${mode}) answerer on peer-b with --warm-hop ==="
  rm -f "${PP_HARD_CGNAT_SHARE_DIR}/${ready_name}"
  pp_hard_exec "${PP_HARD_CGNAT_PEER_B}" rm -f "/share/${ready_name}"

  local hold=$((CYCLES * 20 + 60 + STACK_HOLD_MS / 1000))
  local ans_args=(/probes/${CALL_BIN_NAME} --role answerer --listen "${listen_ma}"
    --advertise-host "${PEER_B_IP}" --ready-file "/share/${ready_name}"
    --hold-seconds "${hold}" --call-id "${call_id}"
    --warm-hop "${HOP_MA_PUBLIC}" --min-rx-frames "${CYCLES}")
  if [[ "${mode}" == "stack" ]]; then
    local watch_ms=$((STACK_HOLD_MS > 4000 ? STACK_HOLD_MS - 3000 : 0))
    ans_args+=(--product-stack --rx-stall-ms "${RX_STALL_MS}" --watch-ms "${watch_ms}")
  fi
  pp_hard_link_clear_container "${PP_HARD_CGNAT_PEER_A}"
  pp_hard_link_clear_container "${PP_HARD_CGNAT_PEER_B}"
  # shellcheck disable=SC2086
  [[ -n "${NETEM_A}" ]] && pp_hard_qdisc_replace "${PP_HARD_CGNAT_PEER_A}" netem ${NETEM_A}
  # shellcheck disable=SC2086
  [[ -n "${NETEM_B}" ]] && pp_hard_qdisc_replace "${PP_HARD_CGNAT_PEER_B}" netem ${NETEM_B}

  pp_hard_exec "${PP_HARD_CGNAT_PEER_B}" "${ans_args[@]}" &
  local ans_pid=$!
  cleanup_ans() {
    kill "${ans_pid}" 2>/dev/null || true
    wait "${ans_pid}" 2>/dev/null || true
  }
  trap cleanup_ans EXIT

  local _
  for _ in $(seq 1 150); do
    if [[ -s "${PP_HARD_CGNAT_SHARE_DIR}/${ready_name}" ]]; then
      break
    fi
    sleep 0.1
  done
  [[ -s "${PP_HARD_CGNAT_SHARE_DIR}/${ready_name}" ]] || pp_hard_die "answerer ready-file not written (warm-hop may have failed)"

  sleep 1

  local peer
  peer="$(head -n1 "${PP_HARD_CGNAT_SHARE_DIR}/${ready_name}" | tr -d '\n')"
  local peer_account=""
  if [[ "${mode}" == "stack" ]]; then
    peer_account="$(sed -n '2p' "${PP_HARD_CGNAT_SHARE_DIR}/${ready_name}" | tr -d '\n')"
    [[ -n "${peer_account}" ]] || pp_hard_die "answerer ready-file missing account line (product-stack)"
  fi
  echo "${label} hop=${HOP_MA_PUBLIC} peer=${peer} cycles=${CYCLES}"

  local off_args=(/probes/${CALL_BIN_NAME} --role offerer --peer "${peer}"
    --via-hop "${HOP_MA_PUBLIC}"
    --cycles "${CYCLES}" --call-id "${call_id}" --timeout-ms 25000)
  case "${mode}" in
    product) off_args+=(--reach product) ;;
    dirty) off_args+=(--reach bridge --force-dial-fail) ;;
    stack) off_args+=(--product-stack --peer-account "${peer_account}" --hold-ms "${STACK_HOLD_MS}"
             --rx-stall-ms "${RX_STALL_MS}" --timeout-ms 45000) ;;
    *) off_args+=(--peer-id-only) ;;
  esac

  set +e
  pp_hard_exec "${PP_HARD_CGNAT_PEER_A}" "${off_args[@]}"
  local off_rc=$?
  set -e

  # Give the answerer a moment to see call_leave and exit 0 on its own (143 = it never did).
  local grace
  for grace in $(seq 1 20); do
    kill -0 "${ans_pid}" 2>/dev/null || break
    sleep 0.5
  done
  kill "${ans_pid}" 2>/dev/null || true
  set +e
  wait "${ans_pid}"
  local ans_rc=$?
  set -e
  trap - EXIT

  echo "offerer_rc=${off_rc} answerer_rc=${ans_rc}"

  local call_ok=0
  if [[ "${off_rc}" -eq 0 ]]; then
    if [[ "${ans_rc}" -eq 0 || "${ans_rc}" -eq 143 || "${ans_rc}" -eq 137 ]]; then
      call_ok=1
    fi
  fi

  if [[ "${CALL_EXPECT}" == "fail" ]]; then
    if [[ "${call_ok}" -eq 1 ]]; then
      pp_hard_die "${label}: expected REPRODUCE (call fail) but call succeeded — flip PP_HARD_NAT_CALL_EXPECT=success?"
    fi
    echo "ok  REPRODUCED: dual-NAT ${mode} call failed (dogfood signal)"
    echo "${label} smoke PASSED (expect=fail / reproduced)"
    return 0
  fi

  if [[ "${call_ok}" -ne 1 ]]; then
    echo "error: ${label} ${mode} call failed under dual-NAT (offerer_rc=${off_rc} answerer_rc=${ans_rc})" >&2
    echo "hint: may reproduce dogfood; re-run with --expect-call fail to lock reproduce mode" >&2
    return 1
  fi

  echo "ok  dual-NAT ${mode} call Invite→RX→Leave"
  echo "${label} smoke PASSED"
  return 0
}

run_phase() {
  local want="$1"
  case "${PHASE}" in
    all) return 0 ;;
    both)
      if [[ "${want}" == "circuit" || "${want}" == "product" ]]; then
        return 0
      fi
      return 1
      ;;
    "${want}") return 0 ;;
    *) return 1 ;;
  esac
}

if run_phase circuit; then
  run_nat_call "B-HARD-CALL-NAT" "pp-hard-call-nat" "call-nat.ready" \
    "${PP_HARD_NAT_CALL_LISTEN:-/ip4/0.0.0.0/udp/47160/adp/1.0.0}" circuit
fi

if run_phase product; then
  run_nat_call "B-HARD-CALL-NAT-PRODUCT" "pp-hard-call-nat-product" "call-nat-product.ready" \
    "${PP_HARD_NAT_PRODUCT_LISTEN:-/ip4/0.0.0.0/udp/47162/adp/1.0.0}" product
fi

if run_phase dirty; then
  run_nat_call "B-HARD-CALL-NAT-DIRTY" "pp-hard-call-nat-dirty" "call-nat-dirty.ready" \
    "${PP_HARD_NAT_DIRTY_LISTEN:-/ip4/0.0.0.0/udp/47164/adp/1.0.0}" dirty
fi

if run_phase stack; then
  run_nat_call "B-HARD-CALL-NAT-STACK" "pp-hard-call-nat-stack" "call-nat-stack.ready" \
    "${PP_HARD_NAT_STACK_LISTEN:-/ip4/0.0.0.0/udp/47166/adp/1.0.0}" stack
fi

echo "N-HARD-CGNAT-ISH + NAT call phase=${PHASE} PASSED"
