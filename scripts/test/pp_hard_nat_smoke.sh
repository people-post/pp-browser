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
# B-HARD-CALL-NAT-UPGRADE — Phase-9 (call-path-resilience k7): gateways switched to cone NAT and
#                           the gateway↔gateway path blocked, so the call starts relayed; the path
#                           opens, the offerer's direct-upgrade punch moves the live call onto it
#                           (make-before-break); then the direct path is blackholed mid-call and
#                           the call must fail over to its warm relayed standby with audio flowing
# B-HARD-CALL-NAT-PUNCH   — Phase-10 (k7): cone NAT, nothing blocked: the call-start punch lands and
#                           media rides the punched link. Both ends dialing produces two
#                           associations; the dual-dial election may drop the one the call bound
#                           first — the call must move to the winner without ever showing
#                           Reconnecting, and never fall back to the relay
# B-HARD-CALL-NAT-FLIP    — Phase-11 (k5/k6): cone NAT; the call starts direct (call-start punch)
#                           and keeps a relayed standby (K003); then peer-a moves to another private
#                           address mid-call (a phone changing network: new NAT mapping, the direct
#                           path dies). Its NetworkMonitor sees the change, Amp drops the dead link
#                           (network-changed) and the call fails over onto the standby — never
#                           Reconnecting
# B-HARD-CALL-NAT-MOBILE  — Phase-12 (k6): cone NAT (a punch would land), the offerer pinned
#                           `--mobility mobile`: the pair anchors on the relay — the answerer awaits
#                           the circuit without punching (it learned the class from the invite's
#                           caps), the offerer never punches for an upgrade, media stays relayed
# Cold phases also require >= COLD_MIN_RX audio frames received on BOTH sides.
#
# NAT: gateways default to symmetric mapping (punching can never land, so the phases above stay
# relay-shaped); the upgrade phase flips them to cone and back (hard-gw-entrypoint.sh).
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
# circuit | stack | cold | cold-dirty | cold-await | upgrade | broadcast | all
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

  PHASE: circuit|stack|cold|cold-dirty|cold-await|upgrade|punch|flip|mobile|broadcast|all
    all    = circuit + stack + cold + cold-dirty + cold-await + upgrade + punch + flip + mobile + broadcast (default)
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
  circuit|stack|cold|cold-dirty|cold-await|upgrade|punch|flip|mobile|broadcast|all) ;;
  product|dirty|both)
    pp_hard_die "--phase ${PHASE} was retired (probe-local reach copies); use cold / cold-dirty / cold-await" ;;
  *) pp_hard_die "--phase must be circuit|stack|cold|cold-dirty|cold-await|upgrade|punch|flip|mobile|broadcast|all (got ${PHASE})" ;;
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
# Upgrade phase: long enough for the +20 s upgrade attempt, the blackhole and the failover.
UPGRADE_HOLD_MS="${PP_HARD_NAT_UPGRADE_HOLD_MS:-45000}"
# Flip phase: hold past the flip + reconnect; the RX-stall gate is the recovery budget.
FLIP_HOLD_MS="${PP_HARD_NAT_FLIP_HOLD_MS:-35000}"
FLIP_RX_STALL_MS="${PP_HARD_NAT_FLIP_RX_STALL_MS:-12000}"
# Mobile phase: past the +3 s and +20 s upgrade slots a stationary pair would use.
MOBILE_HOLD_MS="${PP_HARD_NAT_MOBILE_HOLD_MS:-25000}"

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

# wait_log <file> <ere> <seconds> — 0 once a line matches, 1 on timeout.
wait_log() {
  local file="$1" ere="$2" secs="$3" _
  for _ in $(seq 1 $((secs * 10))); do
    grep -qE "${ere}" "${file}" 2>/dev/null && return 0
    sleep 0.1
  done
  return 1
}

# Upgrade-phase choreography, run beside the offerer: open the blocked path once the first
# upgrade attempt missed on it, wait for the call to move onto the direct link, then blackhole it.
# Progress lands in <marks>; the asserts read it after the call.
# upgrade_choreo <offerer_log> <answerer_log> <marks>
upgrade_choreo() {
  local off_log="$1" ans_log="$2" marks="$3"
  wait_log "${off_log}" '\[CallMediaBridge\] direct upgrade miss' 30 || { echo "no-first-miss" >>"${marks}"; return; }
  echo "first-miss" >>"${marks}"
  pp_hard_cgnat_block_p2p off
  wait_log "${off_log}" 'call-media path migrated .* path=direct' 45 || { echo "no-upgrade" >>"${marks}"; return; }
  echo "upgraded" >>"${marks}"
  # Settled on direct: the relay path released to standby. (A punched link can lose the dual-dial
  # election right after the switch; the call then goes back to the relay and re-migrates later.)
  wait_log "${off_log}" 'path released to standby .* path=relayed' 30 || { echo "no-standby" >>"${marks}"; return; }
  sleep 1
  pp_hard_cgnat_block_p2p on
  echo "blackholed" >>"${marks}"
}

# Flip-phase choreography: once audio flows on the direct path, move peer-a to another address,
# then wait for the call to land on a new path. <marks> gets "flipped <line>" (offerer log length
# at the flip) and "recovered <seconds>".
# flip_choreo <offerer_log> <marks>
flip_choreo() {
  local off_log="$1" marks="$2" _ start
  for _ in $(seq 1 300); do
    [[ "$(max_rx "${off_log}")" -ge 100 ]] && break
    sleep 0.1
  done
  [[ "$(max_rx "${off_log}")" -ge 100 ]] || { echo "no-audio" >>"${marks}"; return; }
  # k6: the call keeps a relayed standby next to its direct path — flip once it is up.
  for _ in $(seq 1 150); do
    grep -q 'relay standby up' "${off_log}" && break
    sleep 0.1
  done
  grep -q 'relay standby up' "${off_log}" && echo "standby" >>"${marks}"
  sleep 1
  local line
  line="$(wc -l <"${off_log}")"
  pp_hard_cgnat_flip_peer_a_addr away
  start="$(date +%s.%N)"
  echo "flipped ${line}" >>"${marks}"
  for _ in $(seq 1 300); do
    if tail -n +"$((line + 1))" "${off_log}" | grep -qE 'CallMediaLeg reconnected|CallMediaLeg failover|call-media path migrated'; then
      echo "recovered $(awk -v a="$(date +%s.%N)" -v b="${start}" 'BEGIN { printf "%.1f", a - b }')" >>"${marks}"
      return
    fi
    sleep 0.1
  done
  echo "not-recovered" >>"${marks}"
}

# assert_flip <label> <offerer_log> <marks>
assert_flip() {
  local label="$1" off_log="$2" marks="$3" line
  line="$(sed -n 's/^flipped //p' "${marks}")"
  [[ -n "${line}" ]] || pp_hard_die "${label}: the call never had audio to flip under ($(cat "${marks}"))"
  local after
  after="$(tail -n +"$((line + 1))" "${off_log}")"
  grep -qE '\[NetworkChange\] network change gen=[0-9]+ .*attachment_changed=1' <<<"${after}" ||
    pp_hard_die "${label}: the offerer's NetworkMonitor never reported the address change"
  echo "ok  flip: NetworkMonitor saw the move ($(grep -oE 'network change gen=[0-9]+ online=[^ ]+' <<<"${after}" | head -1))"
  # The dead direct link goes either way: the network-change probe (2 s), or at once when a send in
  # the address gap fails at the OS (unreachable).
  grep -qE 'MeshLink\] link dropped .*path=punched.*reason=(network-changed|transport-failed)' <<<"${after}" ||
    pp_hard_die "${label}: the dead direct link was not dropped (network-change probe / unreachable send)"
  echo "ok  flip: dead link dropped ($(grep -oE 'path=punched.*reason=(network-changed|transport-failed)' <<<"${after}" | head -1 | sed 's/.*reason=//'))"
  grep -qx 'not-recovered' "${marks}" && pp_hard_die "${label}: the call never moved to a new path after the flip"
  grep -qx 'standby' "${marks}" || pp_hard_die "${label}: no relay standby came up before the flip (K003)"
  grep -q 'CallMediaLeg failover .* to=relayed' <<<"${after}" ||
    pp_hard_die "${label}: the call did not fail over onto its relay standby"
  ! grep -qE '\[CallLifecycle\] status=[A-Za-z]+->Reconnecting' <<<"${after}" ||
    pp_hard_die "${label}: the call showed Reconnecting although it had a standby"
  echo "ok  flip: failover onto the relay standby $(sed -n 's/^recovered //p' "${marks}") s after the flip, never Reconnecting"
}

# assert_mobile <label> <offerer_log> <answerer_log>
assert_mobile() {
  local label="$1" off_log="$2" ans_log="$3"
  grep -q 'mobility pinned to mobile' "${off_log}" || pp_hard_die "${label}: --mobility mobile not applied"
  grep -q 'await without punch (path policy)' "${ans_log}" ||
    pp_hard_die "${label}: the answerer punched (it did not learn the offerer's class from the invite)"
  ! grep -q 'direct upgrade attempt' "${off_log}" || pp_hard_die "${label}: the offerer punched for an upgrade"
  ! grep -qE 'call-media path migrated .* path=direct' "${off_log}" "${ans_log}" ||
    pp_hard_die "${label}: media moved onto a direct path"
  echo "ok  mobile: relay anchor — no call-start punch, no upgrade, media stayed relayed"
  local off_rx ans_rx
  off_rx="$(max_rx "${off_log}")"
  ans_rx="$(max_rx "${ans_log}")"
  [[ "${off_rx}" -ge "${COLD_MIN_RX}" && "${ans_rx}" -ge "${COLD_MIN_RX}" ]] ||
    pp_hard_die "${label}: audio short offerer_rx=${off_rx} answerer_rx=${ans_rx}"
  echo "ok  mobile audio both ways offerer_rx=${off_rx} answerer_rx=${ans_rx}"
}

# assert_upgrade <label> <offerer_log> <answerer_log> <marks>
assert_upgrade() {
  local label="$1" off_log="$2" ans_log="$3" marks="$4"
  grep -qx first-miss "${marks}" || pp_hard_die "${label}: the first upgrade attempt never ran on the blocked path"
  grep -qx upgraded "${marks}" || pp_hard_die "${label}: the call never moved onto the direct path once it opened"
  grep -q 'CallMediaLeg migrate switched .* path=direct' "${ans_log}" ||
    pp_hard_die "${label}: the answerer never switched onto the direct path"
  echo "ok  upgrade: relayed → direct (make-before-break) on both sides"
  grep -qx no-standby "${marks}" && pp_hard_die "${label}: the call never settled on direct with the relay as standby"
  grep -qx blackholed "${marks}" || pp_hard_die "${label}: the direct path was never blackholed"
  grep -qE 'CallMediaLeg failover .* to=relayed' "${off_log}" "${ans_log}" ||
    pp_hard_die "${label}: no failover onto the relayed standby after the direct path died"
  echo "ok  failover: direct blackholed → warm relayed standby ($(grep -hoE 'CallMediaLeg failover .* to=[a-z]+' "${off_log}" "${ans_log}" | head -1 | sed 's/.*reason=//'))"
}

# assert_punch <label> <offerer_log> <answerer_log>
assert_punch() {
  local label="$1" off_log="$2" ans_log="$3"
  grep -qE '\[PeerReach\] (peer reachable via circuit/punch .*path=punched|peer connected .*path=direct)' \
    "${off_log}" "${ans_log}" || pp_hard_die "${label}: the call-start punch never connected the peers"
  echo "ok  punch: peers connected over a punched link"
  ! grep -qE 'call-media path migrated .* path=circuit' "${off_log}" "${ans_log}" ||
    pp_hard_die "${label}: media fell back onto the relay"
  ! grep -qE '\[CallLifecycle\] status=[A-Za-z]+->Reconnecting' "${off_log}" "${ans_log}" ||
    pp_hard_die "${label}: the call showed Reconnecting (a dual-dial drop must rebind quietly)"
  local rebinds
  rebinds="$(cat "${off_log}" "${ans_log}" | grep -c 'CallMediaLeg reconnected .*(quiet rebind)' || true)"
  echo "ok  punch: media on the punched link, never Reconnecting (quiet rebinds=${rebinds})"
  local off_rx ans_rx
  off_rx="$(max_rx "${off_log}")"
  ans_rx="$(max_rx "${ans_log}")"
  [[ "${off_rx}" -ge "${COLD_MIN_RX}" && "${ans_rx}" -ge "${COLD_MIN_RX}" ]] ||
    pp_hard_die "${label}: audio short offerer_rx=${off_rx} answerer_rx=${ans_rx} (< ${COLD_MIN_RX})"
  echo "ok  punch audio both ways offerer_rx=${off_rx} answerer_rx=${ans_rx}"
}

# run_nat_call <label> <call_id> <ready_name> <listen_ma> <mode>
# mode: circuit | stack | cold | cold-dirty | cold-await | cold-upgrade | cold-punch
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
  [[ "${mode}" == "cold-upgrade" ]] && hold_ms="${UPGRADE_HOLD_MS}"
  if [[ "${mode}" == "cold-flip" ]]; then
    hold_ms="${FLIP_HOLD_MS}"
    stall_ms="${FLIP_RX_STALL_MS}"
  fi
  [[ "${mode}" == "cold-mobile" ]] && hold_ms="${MOBILE_HOLD_MS}"
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
    cold|cold-dirty|cold-await|cold-upgrade|cold-punch|cold-flip|cold-mobile)
      off_args+=(--product-stack --peer-account "${peer_account}" --hold-ms "${hold_ms}"
                 --rx-stall-ms "${stall_ms}" --timeout-ms $((hold_ms + 60000)) --signal-dir "${signal_dir}")
      [[ "${mode}" == "cold-dirty" ]] && off_args+=(--dirty-book --force-dial-fail)
      [[ "${mode}" == "cold-mobile" ]] && off_args+=(--mobility mobile) ;;
    *) off_args+=(--peer-id-only) ;;
  esac

  local marks="${LOG_DIR}/${call_id}.marks" choreo_pid=""
  if [[ "${mode}" == "cold-upgrade" ]]; then
    : >"${marks}"
    upgrade_choreo "${off_log}" "${ans_log}" "${marks}" &
    choreo_pid=$!
  elif [[ "${mode}" == "cold-flip" ]]; then
    : >"${marks}"
    flip_choreo "${off_log}" "${marks}" &
    choreo_pid=$!
  fi

  set +e
  pp_hard_exec "${PP_HARD_CGNAT_PEER_A}" "${off_args[@]}" > >(tee "${off_log}") 2>&1
  local off_rc=$?
  set -e
  if [[ -n "${choreo_pid}" ]]; then
    kill "${choreo_pid}" 2>/dev/null || true
    wait "${choreo_pid}" 2>/dev/null || true
  fi

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

  if [[ "${mode}" == "cold-punch" ]]; then
    sleep 0.5
    assert_punch "${label}" "${off_log}" "${ans_log}"
  elif [[ "${mode}" == "cold-mobile" ]]; then
    sleep 0.5
    assert_mobile "${label}" "${off_log}" "${ans_log}"
  elif [[ "${mode}" == "cold-flip" ]]; then
    sleep 0.5
    assert_flip "${label}" "${off_log}" "${marks}"
    local off_rx ans_rx
    off_rx="$(max_rx "${off_log}")"
    ans_rx="$(max_rx "${ans_log}")"
    [[ "${off_rx}" -ge "${COLD_MIN_RX}" && "${ans_rx}" -ge "${COLD_MIN_RX}" ]] ||
      pp_hard_die "${label}: audio short offerer_rx=${off_rx} answerer_rx=${ans_rx}"
    echo "ok  flip audio both ways offerer_rx=${off_rx} answerer_rx=${ans_rx}"
  elif [[ "${mode}" == cold* ]]; then
    sleep 0.5  # let the tee'd answerer log flush
    assert_cold_reach "${label}" "${mode}" "${off_log}" "${ans_log}"
  fi
  [[ "${mode}" == "cold-upgrade" ]] && assert_upgrade "${label}" "${off_log}" "${ans_log}" "${marks}"
  echo "ok  dual-NAT ${mode} call Invite→RX→Leave"
  echo "${label} smoke PASSED"
  return 0
}

# B-HARD-BCAST-NAT (media-client-layers l5c): product BroadcastHub on both ends. The broadcaster
# (peer-a, behind gw-a) goes live through the hop's media_relay; two viewers (peer-b, behind gw-b)
# fetch tickets from the NATed publisher (circuit via the hop), attach receive-only and must decode
# BCAST_MIN_RX audio frames each. The Live tip rides /share (announce push is Spine D).
BCAST_MIN_RX="${PP_HARD_NAT_BCAST_MIN_RX:-100}"
run_nat_broadcast() {
  local label="B-HARD-BCAST-NAT"
  local ready="bcast.ready"
  pp_hard_kill_peer_probes
  echo "=== ${label} broadcaster on peer-a, 2 viewers on peer-b (min rx ${BCAST_MIN_RX}) ==="
  rm -f "${PP_HARD_CGNAT_SHARE_DIR}/${ready}"
  pp_hard_exec "${PP_HARD_CGNAT_PEER_A}" rm -f "/share/${ready}"
  pp_hard_link_clear_container "${PP_HARD_CGNAT_PEER_A}"
  pp_hard_link_clear_container "${PP_HARD_CGNAT_PEER_B}"

  local bc_log="${LOG_DIR}/bcast.broadcaster.log"
  pp_hard_exec "${PP_HARD_CGNAT_PEER_A}" /probes/${CALL_BIN_NAME} --role broadcaster \
    --listen "${PP_HARD_NAT_BCAST_LISTEN:-/ip4/0.0.0.0/udp/47180/adp/1.0.0}" --advertise-host "${PEER_A_IP}" \
    --warm-hop "${HOP_MA_PUBLIC}" --ready-file "/share/${ready}" --hold-seconds 90 --min-rx-frames 100 \
    > >(tee "${bc_log}") 2>&1 &
  local bc_pid=$!
  cleanup_bc() {
    kill "${bc_pid}" 2>/dev/null || true
    wait "${bc_pid}" 2>/dev/null || true
  }
  trap cleanup_bc EXIT

  local _
  for _ in $(seq 1 300); do
    [[ -s "${PP_HARD_CGNAT_SHARE_DIR}/${ready}" ]] && break
    sleep 0.1
  done
  [[ -s "${PP_HARD_CGNAT_SHARE_DIR}/${ready}" ]] || pp_hard_die "${label}: broadcaster never went live"
  echo "${label} live: $(sed -n '4p' "${PP_HARD_CGNAT_SHARE_DIR}/${ready}") via hop=${HOP_MA_PUBLIC}"

  local v pids=() logs=()
  for v in 1 2; do
    local v_log="${LOG_DIR}/bcast.viewer${v}.log"
    logs+=("${v_log}")
    pp_hard_exec "${PP_HARD_CGNAT_PEER_B}" /probes/${CALL_BIN_NAME} --role viewer \
      --listen "/ip4/0.0.0.0/udp/$((47182 + 2 * v))/adp/1.0.0" --advertise-host "${PEER_B_IP}" \
      --warm-hop "${HOP_MA_PUBLIC}" --announce-file "/share/${ready}" --timeout-ms 45000 \
      --min-rx-frames "${BCAST_MIN_RX}" > >(tee "${v_log}") 2>&1 &
    pids+=($!)
  done
  local rc=0 i
  for i in 0 1; do
    set +e
    wait "${pids[$i]}"
    local v_rc=$?
    set -e
    echo "viewer$((i + 1))_rc=${v_rc} rx=$(max_rx "${logs[$i]}")"
    [[ "${v_rc}" -eq 0 ]] || rc=1
  done
  if grep -qE "broadcast failed|^error" "${bc_log}"; then
    echo "error: ${label}: broadcaster reported a failure" >&2
    rc=1
  fi
  cleanup_bc
  trap - EXIT
  if [[ "${rc}" -ne 0 ]]; then
    echo "error: ${label} broadcast failed under dual-NAT" >&2
    return 1
  fi
  echo "ok  dual-NAT broadcast: publisher → hop media_relay → 2 viewers (ticket via circuit)"
  echo "${label} smoke PASSED"
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

if run_phase upgrade; then
  pp_hard_cgnat_set_nat cone
  pp_hard_cgnat_block_p2p on
  set +e
  run_nat_call "B-HARD-CALL-NAT-UPGRADE" "pp-hard-call-nat-upgrade" "call-nat-upgrade.ready" \
    "${PP_HARD_NAT_UPGRADE_LISTEN:-/ip4/0.0.0.0/udp/47174/adp/1.0.0}" cold-upgrade
  upgrade_rc=$?
  set -e
  pp_hard_cgnat_block_p2p off
  pp_hard_cgnat_set_nat symmetric
  [[ "${upgrade_rc}" -eq 0 ]] || exit "${upgrade_rc}"
fi

if run_phase punch; then
  pp_hard_cgnat_set_nat cone
  pp_hard_cgnat_block_p2p off
  set +e
  run_nat_call "B-HARD-CALL-NAT-PUNCH" "pp-hard-call-nat-punch" "call-nat-punch.ready" \
    "${PP_HARD_NAT_PUNCH_LISTEN:-/ip4/0.0.0.0/udp/47176/adp/1.0.0}" cold-punch
  punch_rc=$?
  set -e
  pp_hard_cgnat_set_nat symmetric
  [[ "${punch_rc}" -eq 0 ]] || exit "${punch_rc}"
fi

if run_phase flip; then
  pp_hard_cgnat_set_nat cone
  pp_hard_cgnat_block_p2p off
  set +e
  run_nat_call "B-HARD-CALL-NAT-FLIP" "pp-hard-call-nat-flip" "call-nat-flip.ready" \
    "${PP_HARD_NAT_FLIP_LISTEN:-/ip4/0.0.0.0/udp/47178/adp/1.0.0}" cold-flip
  flip_rc=$?
  set -e
  pp_hard_cgnat_flip_peer_a_addr back
  pp_hard_cgnat_set_nat symmetric
  [[ "${flip_rc}" -eq 0 ]] || exit "${flip_rc}"
fi

if run_phase mobile; then
  pp_hard_cgnat_set_nat cone
  pp_hard_cgnat_block_p2p off
  set +e
  run_nat_call "B-HARD-CALL-NAT-MOBILE" "pp-hard-call-nat-mobile" "call-nat-mobile.ready" \
    "${PP_HARD_NAT_MOBILE_LISTEN:-/ip4/0.0.0.0/udp/47190/adp/1.0.0}" cold-mobile
  mobile_rc=$?
  set -e
  pp_hard_cgnat_set_nat symmetric
  [[ "${mobile_rc}" -eq 0 ]] || exit "${mobile_rc}"
fi

if run_phase broadcast; then
  run_nat_broadcast
fi

echo "N-HARD-CGNAT-ISH + NAT call phase=${PHASE} PASSED"
