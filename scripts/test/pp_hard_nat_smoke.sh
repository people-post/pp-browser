#!/usr/bin/env bash
# Wave 5 CGNAT-ish: dual SNAT peers + public hop.
#
# N-HARD-CGNAT-ISH     — topology asserts (A↛B, hop↛peer-private, peers→hop via SNAT)
# B-HARD-CALL-NAT      — Phase-1: forced nested circuit (--via-hop --peer-id-only)
# (Phase-2 PRODUCT / Phase-3 DIRTY retired: they ran probe-local copies of the reach logic;
#  COLD / COLD-DIRTY / COLD-AWAIT drive the product PeerReachCoordinator instead.)
# B-HARD-CALL-NAT-STACK   — Phase-4: Invite/Accept control + dirty-book media (--product-stack)
# B-HARD-CALL-NAT-COLD    — Phase-5: product stack, signaling via /share (relay-inbox stand-in),
#                           so media reach starts with NO peer link: offerer PeerReachCoordinator
#                           Reach (seed park → circuit), answerer Await (punch, wait for circuit)
# B-HARD-CALL-NAT-COLD-DIRTY — Phase-6: as COLD with the answerer's private MA in the offerer's
#                           dial book and one forced dial miss (backoff left armed) — H010: park
#                           before the private dial, then skip it; the product heals the backoff
# B-HARD-CALL-NAT-COLD-AWAIT — Phase-7: as COLD with the offerer's uplink delayed, so the
#                           answerer's media starts before the offerer's circuit lands and its
#                           Await reach runs cold (dogfood: answerer waiting on the caller)
# Cold phases also require >= COLD_MIN_RX audio frames received on BOTH sides.
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
# circuit | stack | cold | cold-dirty | cold-await | all
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

  PHASE: circuit|stack|cold|cold-dirty|cold-await|all
    all    = circuit + stack + cold + cold-dirty + cold-await (default)
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
  circuit|stack|cold|cold-dirty|cold-await|all) ;;
  product|dirty|both)
    pp_hard_die "--phase ${PHASE} was retired (probe-local reach copies); use cold / cold-dirty / cold-await" ;;
  *) pp_hard_die "--phase must be circuit|stack|cold|cold-dirty|cold-await|all (got ${PHASE})" ;;
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

LOG_DIR="$(mktemp -d)"

# Cold phases: hold / stall / audio gates (override via env).
COLD_HOLD_MS="${PP_HARD_NAT_COLD_HOLD_MS:-8000}"
COLD_RX_STALL_MS="${PP_HARD_NAT_COLD_RX_STALL_MS:-3000}"
COLD_MIN_RX="${PP_HARD_NAT_COLD_MIN_RX:-100}"
# Offerer uplink delay for cold-await: its circuit must land after the answerer's media start.
COLD_AWAIT_NETEM="${PP_HARD_NAT_COLD_AWAIT_NETEM:-delay 250ms}"

# Highest "flow <role> ... rx=N" in a probe log (0 when none).
max_rx() {
  local n
  n="$(grep -oE '^flow [a-z]+ t=[0-9.]+s rx=[0-9]+' "$1" | sed 's/.*rx=//' | sort -n | tail -1 || true)"
  echo "${n:-0}"
}

# Cold phases must prove the product reach ran from scratch — not the reuse shortcut — and
# that audio flowed both ways.
# assert_cold_reach <label> <mode> <offerer_log> <answerer_log>
assert_cold_reach() {
  local label="$1" mode="$2" off_log="$3" ans_log="$4"
  local settle='\[PeerReach\] (peer reachable|peer connected|EnsureAssociation ok)'
  grep -q '\[PeerReach\] reach start .*mode=reach' "${off_log}" ||
    pp_hard_die "${label}: offerer PeerReachCoordinator never started a cold reach (reuse shortcut?)"
  case "${mode}" in
    cold-dirty)
      grep -q 'force-dial-fail peer=.*backoff left armed\|force-dial-fail peer=.*aborted' "${off_log}" ||
        pp_hard_die "${label}: forced dial miss did not happen (dirty book not poisoned)"
      grep -q '\[PeerReach\] skip EnsureAssociation private Preferred' "${off_log}" ||
        pp_hard_die "${label}: dirty book did not drive the H010 skip-private-Preferred branch" ;;
    cold-await)
      grep -q '\[PeerReach\] reach start .*mode=await' "${ans_log}" ||
        pp_hard_die "${label}: answerer reused a link — its Await reach never ran cold (raise the delay?)" ;;
  esac
  echo "ok  cold reach offerer: $(grep -E "${settle}" "${off_log}" | head -1 | sed 's/.*\[PeerReach\] //' || true)"
  # Outside cold-await the answerer may legitimately need no reach: it reuses the offerer's link,
  # or joins the offerer's bundle that is already live. The audio gate below proves it connected.
  local ans_path
  ans_path="$(grep -E "${settle}" "${ans_log}" | head -1 | sed 's/.*\[PeerReach\] //' || true)"
  if [[ -z "${ans_path}" ]] && grep -q 'Media started with existing inbound stream' "${ans_log}"; then
    ans_path="joined the offerer's live inbound bundle (no reach needed)"
  fi
  echo "ok  cold reach answerer: ${ans_path:-(none)}"
  local off_rx ans_rx
  off_rx="$(max_rx "${off_log}")"
  ans_rx="$(max_rx "${ans_log}")"
  [[ "${off_rx}" -ge "${COLD_MIN_RX}" ]] || pp_hard_die "${label}: offerer rx=${off_rx} < ${COLD_MIN_RX} frames"
  [[ "${ans_rx}" -ge "${COLD_MIN_RX}" ]] || pp_hard_die "${label}: answerer rx=${ans_rx} < ${COLD_MIN_RX} frames"
  echo "ok  cold audio both ways offerer_rx=${off_rx} answerer_rx=${ans_rx}"
}

# run_nat_call <label> <call_id> <ready_name> <listen_ma> <mode>
# mode: circuit | stack | cold | cold-dirty | cold-await
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
  local product_stack=0
  [[ "${mode}" == "stack" || "${mode}" == cold* ]] && product_stack=1
  local signal_dir="/share/sig-${call_id}"
  local hold_ms="${STACK_HOLD_MS}" stall_ms="${RX_STALL_MS}"
  if [[ "${mode}" == cold* ]]; then
    hold_ms="${COLD_HOLD_MS}"
    stall_ms="${COLD_RX_STALL_MS}"
  fi
  if [[ "${product_stack}" -eq 1 ]]; then
    local watch_ms=$((hold_ms > 4000 ? hold_ms - 3000 : 0))
    ans_args+=(--product-stack --rx-stall-ms "${stall_ms}" --watch-ms "${watch_ms}")
  fi
  if [[ "${mode}" == cold* ]]; then
    pp_hard_exec "${PP_HARD_CGNAT_PEER_B}" rm -rf "${signal_dir}"
    ans_args+=(--signal-dir "${signal_dir}")
  fi
  local ans_log="${LOG_DIR}/${call_id}.answerer.log"
  local off_log="${LOG_DIR}/${call_id}.offerer.log"
  pp_hard_link_clear_container "${PP_HARD_CGNAT_PEER_A}"
  pp_hard_link_clear_container "${PP_HARD_CGNAT_PEER_B}"
  local netem_a="${NETEM_A}"
  [[ "${mode}" == "cold-await" && -z "${netem_a}" ]] && netem_a="${COLD_AWAIT_NETEM}"
  # shellcheck disable=SC2086
  [[ -n "${netem_a}" ]] && pp_hard_qdisc_replace "${PP_HARD_CGNAT_PEER_A}" netem ${netem_a}
  # shellcheck disable=SC2086
  [[ -n "${NETEM_B}" ]] && pp_hard_qdisc_replace "${PP_HARD_CGNAT_PEER_B}" netem ${NETEM_B}

  # tee keeps the console output; $! stays the docker exec pid (its exit code is the answerer's).
  pp_hard_exec "${PP_HARD_CGNAT_PEER_B}" "${ans_args[@]}" > >(tee "${ans_log}") 2>&1 &
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
  if [[ "${product_stack}" -eq 1 ]]; then
    peer_account="$(sed -n '2p' "${PP_HARD_CGNAT_SHARE_DIR}/${ready_name}" | tr -d '\n')"
    [[ -n "${peer_account}" ]] || pp_hard_die "answerer ready-file missing account line (product-stack)"
  fi
  echo "${label} hop=${HOP_MA_PUBLIC} peer=${peer} cycles=${CYCLES}"

  local off_args=(/probes/${CALL_BIN_NAME} --role offerer --peer "${peer}"
    --via-hop "${HOP_MA_PUBLIC}"
    --cycles "${CYCLES}" --call-id "${call_id}" --timeout-ms 25000)
  case "${mode}" in
    stack) off_args+=(--product-stack --peer-account "${peer_account}" --hold-ms "${STACK_HOLD_MS}"
             --rx-stall-ms "${RX_STALL_MS}" --timeout-ms 45000) ;;
    cold|cold-dirty|cold-await)
      off_args+=(--product-stack --peer-account "${peer_account}" --hold-ms "${hold_ms}"
                 --rx-stall-ms "${stall_ms}" --timeout-ms 60000 --signal-dir "${signal_dir}")
      [[ "${mode}" == "cold-dirty" ]] && off_args+=(--dirty-book --force-dial-fail) ;;
    *) off_args+=(--peer-id-only) ;;
  esac

  set +e
  pp_hard_exec "${PP_HARD_CGNAT_PEER_A}" "${off_args[@]}" > >(tee "${off_log}") 2>&1
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

  if [[ "${mode}" == cold* ]]; then
    sleep 0.5  # let the tee'd answerer log flush
    assert_cold_reach "${label}" "${mode}" "${off_log}" "${ans_log}"
  fi
  echo "ok  dual-NAT ${mode} call Invite→RX→Leave"
  echo "${label} smoke PASSED"
  return 0
}

run_phase() {
  local want="$1"
  [[ "${PHASE}" == "all" || "${PHASE}" == "${want}" ]]
}

if run_phase circuit; then
  run_nat_call "B-HARD-CALL-NAT" "pp-hard-call-nat" "call-nat.ready" \
    "${PP_HARD_NAT_CALL_LISTEN:-/ip4/0.0.0.0/udp/47160/adp/1.0.0}" circuit
fi

if run_phase stack; then
  run_nat_call "B-HARD-CALL-NAT-STACK" "pp-hard-call-nat-stack" "call-nat-stack.ready" \
    "${PP_HARD_NAT_STACK_LISTEN:-/ip4/0.0.0.0/udp/47166/adp/1.0.0}" stack
fi

if run_phase cold; then
  run_nat_call "B-HARD-CALL-NAT-COLD" "pp-hard-call-nat-cold" "call-nat-cold.ready" \
    "${PP_HARD_NAT_COLD_LISTEN:-/ip4/0.0.0.0/udp/47168/adp/1.0.0}" cold
fi

if run_phase cold-dirty; then
  run_nat_call "B-HARD-CALL-NAT-COLD-DIRTY" "pp-hard-call-nat-cold-dirty" "call-nat-cold-dirty.ready" \
    "${PP_HARD_NAT_COLD_DIRTY_LISTEN:-/ip4/0.0.0.0/udp/47170/adp/1.0.0}" cold-dirty
fi

if run_phase cold-await; then
  run_nat_call "B-HARD-CALL-NAT-COLD-AWAIT" "pp-hard-call-nat-cold-await" "call-nat-cold-await.ready" \
    "${PP_HARD_NAT_COLD_AWAIT_LISTEN:-/ip4/0.0.0.0/udp/47172/adp/1.0.0}" cold-await
fi

echo "N-HARD-CGNAT-ISH + NAT call phase=${PHASE} PASSED"
