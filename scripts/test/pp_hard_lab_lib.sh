# Shared helpers for hard-lab smokes (N-HARD-FORCE / B-HARD-* / Wave 2 link profiles).
# shellcheck shell=bash
# Source after ROOT is set. Expects Docker + compose env defaults.

: "${ROOT:?ROOT must be set before sourcing pp_hard_lab_lib.sh}"

PP_HARD_COMPOSE_FILE="${PP_HARD_COMPOSE_FILE:-${ROOT}/packaging/pp-node/docker-compose.hard-lab.yml}"
PP_HARD_COMPOSE_PROJECT="${PP_HARD_COMPOSE_PROJECT:-pp-hard-lab}"
PP_HARD_STATUS_URL="${PP_HARD_STATUS_URL:-http://127.0.0.1:18618}"
PP_HARD_PROBE_DIR="${PP_HARD_PROBE_DIR:-${ROOT}/build/src/app/node}"
PP_HARD_SHARE_DIR="${PP_HARD_SHARE_DIR:-/tmp/pp-hard-lab-share}"
PP_HARD_HOP_CONTAINER="${PP_HARD_HOP_CONTAINER:-pp-hard-lab-hop}"
PP_HARD_PEER_A="${PP_HARD_PEER_A:-pp-hard-lab-peer-a}"
PP_HARD_PEER_B="${PP_HARD_PEER_B:-pp-hard-lab-peer-b}"
PP_HARD_NET_A="${PP_HARD_NET_A:-pp-hard-lab-net-a}"
PP_HARD_NET_B="${PP_HARD_NET_B:-pp-hard-lab-net-b}"

# Wave 2 starting knobs (tune with evidence; see HARD_LAB.md link profiles).
PP_HARD_LOSSY_NETEM="${PP_HARD_LOSSY_NETEM:-delay 75ms 15ms distribution normal loss 3%}"
PP_HARD_ASYM_NETEM="${PP_HARD_ASYM_NETEM:-delay 150ms 20ms distribution normal loss 5%}"
PP_HARD_BW_TBF="${PP_HARD_BW_TBF:-rate 512kbit burst 32kb latency 400ms}"

pp_hard_die() { echo "error: $*" >&2; exit 1; }

pp_hard_need_cmd() {
  command -v "$1" >/dev/null 2>&1 || pp_hard_die "missing command: $1"
}

pp_hard_compose() {
  pp_hard_need_cmd docker
  mkdir -p "${PP_HARD_SHARE_DIR}"
  # Compose volume paths must be absolute (relative is resolved from the compose file dir).
  if [[ "${PP_HARD_PROBE_DIR}" != /* ]]; then
    PP_HARD_PROBE_DIR="$(cd "${ROOT}/${PP_HARD_PROBE_DIR}" && pwd)"
  fi
  if [[ "${PP_HARD_SHARE_DIR}" != /* ]]; then
    mkdir -p "${PP_HARD_SHARE_DIR}"
    PP_HARD_SHARE_DIR="$(cd "${PP_HARD_SHARE_DIR}" && pwd)"
  fi
  PP_HARD_PROBE_DIR="${PP_HARD_PROBE_DIR}" PP_HARD_SHARE_DIR="${PP_HARD_SHARE_DIR}" \
    docker compose -p "${PP_HARD_COMPOSE_PROJECT}" -f "${PP_HARD_COMPOSE_FILE}" "$@"
}

pp_hard_container_ip_on_net() {
  local container="$1"
  local net="$2"
  docker inspect -f "{{(index .NetworkSettings.Networks \"${net}\").IPAddress}}" "${container}"
}

pp_hard_hop_peer_id() {
  pp_hard_need_cmd curl
  pp_hard_need_cmd python3
  local auth_args=()
  if [[ -n "${PP_NODE_STATUS_TOKEN:-}" ]]; then
    auth_args=(-H "Authorization: Bearer ${PP_NODE_STATUS_TOKEN}")
  fi
  local status_json
  status_json="$(curl -fsS -m 5 "${auth_args[@]}" "${PP_HARD_STATUS_URL}/status")"
  STATUS_JSON="${status_json}" python3 -c 'import json,os; s=json.loads(os.environ["STATUS_JSON"]); p=s.get("peer_id") or "";
assert p, "missing peer_id"; print(p)'
}

pp_hard_hop_ma_for_ip() {
  local ip="$1"
  local peer="$2"
  printf '/ip4/%s/udp/18517/adp/1.0.0/p2p/%s\n' "${ip}" "${peer}"
}

pp_hard_exec() {
  local container="$1"
  shift
  docker exec "${container}" "$@"
}

pp_hard_wait_healthz() {
  export PP_NODE_STATUS_URL="${PP_HARD_STATUS_URL}"
  bash "${ROOT}/scripts/test/pp_node_image_smoke.sh" --status-url "${PP_HARD_STATUS_URL}"
}

# First non-lo iface in a peer/hop container (usually eth0 on compose bridge).
pp_hard_peer_iface() {
  local container="$1"
  local ifc
  ifc="$(pp_hard_exec "${container}" sh -c \
    "ip -o link show | awk -F': ' '\$2 != \"lo\" { split(\$2, a, \"@\"); print a[1]; exit }'")"
  [[ -n "${ifc}" ]] || pp_hard_die "no non-lo iface in ${container}"
  printf '%s\n' "${ifc}"
}

pp_hard_link_clear_container() {
  local container="$1"
  local ifc
  ifc="$(pp_hard_peer_iface "${container}")"
  pp_hard_exec "${container}" tc qdisc del dev "${ifc}" root 2>/dev/null || true
}

pp_hard_link_clear() {
  pp_hard_link_clear_container "${PP_HARD_PEER_A}"
  pp_hard_link_clear_container "${PP_HARD_PEER_B}"
}

pp_hard_qdisc_replace() {
  local container="$1"
  shift
  local ifc
  ifc="$(pp_hard_peer_iface "${container}")"
  # shellcheck disable=SC2068
  pp_hard_exec "${container}" tc qdisc replace dev "${ifc}" root "$@"
  echo "  ${container} ${ifc}: tc $*"
}

# Apply Wave 2 link profile on peer veths (NET_ADMIN required). clean = clear only.
# lossy: both legs; asym: peer-a only (asymmetric A↔hop vs clean B↔hop); bw: tbf both.
pp_hard_link_apply() {
  local profile="${1:-clean}"
  echo "=== hard-lab link profile=${profile} ==="
  pp_hard_link_clear
  case "${profile}" in
    clean) ;;
    lossy)
      # shellcheck disable=SC2086
      pp_hard_qdisc_replace "${PP_HARD_PEER_A}" netem ${PP_HARD_LOSSY_NETEM}
      # shellcheck disable=SC2086
      pp_hard_qdisc_replace "${PP_HARD_PEER_B}" netem ${PP_HARD_LOSSY_NETEM}
      ;;
    asym)
      # shellcheck disable=SC2086
      pp_hard_qdisc_replace "${PP_HARD_PEER_A}" netem ${PP_HARD_ASYM_NETEM}
      ;;
    bw)
      # shellcheck disable=SC2086
      pp_hard_qdisc_replace "${PP_HARD_PEER_A}" tbf ${PP_HARD_BW_TBF}
      # shellcheck disable=SC2086
      pp_hard_qdisc_replace "${PP_HARD_PEER_B}" tbf ${PP_HARD_BW_TBF}
      ;;
    *)
      pp_hard_die "unknown link profile: ${profile} (clean|lossy|asym|bw)"
      ;;
  esac
}

# Hop still accepting work after an impaired run (no fatal).
pp_hard_assert_hop_alive() {
  curl -fsS -m 5 "${PP_HARD_STATUS_URL}/healthz" >/dev/null \
    || pp_hard_die "hop /healthz failed after link profile (hop fatal?)"
  curl -fsS -m 5 "${PP_HARD_STATUS_URL}/status" >/dev/null \
    || pp_hard_die "hop /status failed after link profile"
  echo "ok  hop still healthy"
}

# Run cmd; on failure clear+re-apply profile and retry once (netem flake policy).
pp_hard_run_with_netem_retry() {
  local profile="$1"
  shift
  if "$@"; then
    return 0
  fi
  echo "warn: impaired run failed once under link=${profile}; retrying once" >&2
  pp_hard_link_apply "${profile}"
  "$@"
}

# Populates: HOP_IP_A HOP_IP_B PEER_A_IP PEER_B_IP HOP_PEER_ID HOP_MA_A HOP_MA_B
pp_hard_resolve_topology() {
  HOP_IP_A="$(pp_hard_container_ip_on_net "${PP_HARD_HOP_CONTAINER}" "${PP_HARD_NET_A}")"
  HOP_IP_B="$(pp_hard_container_ip_on_net "${PP_HARD_HOP_CONTAINER}" "${PP_HARD_NET_B}")"
  PEER_A_IP="$(pp_hard_container_ip_on_net "${PP_HARD_PEER_A}" "${PP_HARD_NET_A}")"
  PEER_B_IP="$(pp_hard_container_ip_on_net "${PP_HARD_PEER_B}" "${PP_HARD_NET_B}")"
  [[ -n "${HOP_IP_A}" && -n "${HOP_IP_B}" ]] || pp_hard_die "hop missing IPs on net_a/net_b"
  [[ -n "${PEER_A_IP}" && -n "${PEER_B_IP}" ]] || pp_hard_die "peers missing IPs"
  HOP_PEER_ID="$(pp_hard_hop_peer_id)"
  HOP_MA_A="$(pp_hard_hop_ma_for_ip "${HOP_IP_A}" "${HOP_PEER_ID}")"
  HOP_MA_B="$(pp_hard_hop_ma_for_ip "${HOP_IP_B}" "${HOP_PEER_ID}")"
}

pp_hard_ensure_up() {
  local skip_up="${1:-0}"
  mkdir -p "${PP_HARD_SHARE_DIR}"
  if [[ "${skip_up}" -eq 0 ]]; then
    echo "=== hard-lab compose up project=${PP_HARD_COMPOSE_PROJECT} ==="
    if [[ ! -f "${ROOT}/dist/pp-node/docker/Dockerfile" ]]; then
      pp_hard_die "missing dist/pp-node/docker; package with scripts/platform/pp_node_package_linux.sh all"
    fi
    pp_hard_compose up -d --build --force-recreate
  fi
  echo "=== wait hop healthz ${PP_HARD_STATUS_URL} ==="
  for _ in $(seq 1 60); do
    if curl -fsS -m 2 "${PP_HARD_STATUS_URL}/healthz" >/dev/null 2>&1; then
      break
    fi
    sleep 0.5
  done
  pp_hard_wait_healthz
  pp_hard_resolve_topology
  # Fresh compose: drop any host-leftover expectation. On --skip-up, preserve
  # caller-applied Wave 2 qdiscs (pp_hard_link_smoke applies after ensure_up).
  if [[ "${skip_up}" -eq 0 ]]; then
    pp_hard_link_clear 2>/dev/null || true
  fi
  echo "hop peer_id=${HOP_PEER_ID}"
  echo "hop on net_a: ${HOP_IP_A}  net_b: ${HOP_IP_B}"
  echo "peer-a=${PEER_A_IP}  peer-b=${PEER_B_IP}"
}

# --- Wave 5 CGNAT-ish (dual SNAT) ---------------------------------------------
# Separate compose family: packaging/pp-node/docker-compose.hard-lab-cgnat.yml
# Fixed addressing (see compose):
#   public 10.117.0.0/24  hop=.2  gw-a=.10  gw-b=.11
#   priv-a 10.117.1.0/24  gw=.1   peer-a=.10
#   priv-b 10.117.2.0/24  gw=.1   peer-b=.10

PP_HARD_CGNAT_COMPOSE_FILE="${PP_HARD_CGNAT_COMPOSE_FILE:-${ROOT}/packaging/pp-node/docker-compose.hard-lab-cgnat.yml}"
PP_HARD_CGNAT_COMPOSE_PROJECT="${PP_HARD_CGNAT_COMPOSE_PROJECT:-pp-hard-lab-cgnat}"
PP_HARD_CGNAT_STATUS_URL="${PP_HARD_CGNAT_STATUS_URL:-http://127.0.0.1:18628}"
PP_HARD_CGNAT_SHARE_DIR="${PP_HARD_CGNAT_SHARE_DIR:-/tmp/pp-hard-lab-cgnat-share}"
PP_HARD_CGNAT_HOP="${PP_HARD_CGNAT_HOP:-pp-hard-lab-cgnat-hop}"
PP_HARD_CGNAT_PEER_A="${PP_HARD_CGNAT_PEER_A:-pp-hard-lab-cgnat-peer-a}"
PP_HARD_CGNAT_PEER_B="${PP_HARD_CGNAT_PEER_B:-pp-hard-lab-cgnat-peer-b}"
PP_HARD_CGNAT_GW_A="${PP_HARD_CGNAT_GW_A:-pp-hard-lab-cgnat-gw-a}"
PP_HARD_CGNAT_GW_B="${PP_HARD_CGNAT_GW_B:-pp-hard-lab-cgnat-gw-b}"
PP_HARD_CGNAT_NET_PUBLIC="${PP_HARD_CGNAT_NET_PUBLIC:-pp-hard-lab-cgnat-net-public}"
PP_HARD_CGNAT_NET_PRIV_A="${PP_HARD_CGNAT_NET_PRIV_A:-pp-hard-lab-cgnat-net-priv-a}"
PP_HARD_CGNAT_NET_PRIV_B="${PP_HARD_CGNAT_NET_PRIV_B:-pp-hard-lab-cgnat-net-priv-b}"
PP_HARD_CGNAT_HOP_IP="${PP_HARD_CGNAT_HOP_IP:-10.117.0.2}"
PP_HARD_CGNAT_PEER_A_IP="${PP_HARD_CGNAT_PEER_A_IP:-10.117.1.10}"
PP_HARD_CGNAT_PEER_B_IP="${PP_HARD_CGNAT_PEER_B_IP:-10.117.2.10}"
PP_HARD_CGNAT_GW_A_PRIV_IP="${PP_HARD_CGNAT_GW_A_PRIV_IP:-10.117.1.254}"
PP_HARD_CGNAT_GW_B_PRIV_IP="${PP_HARD_CGNAT_GW_B_PRIV_IP:-10.117.2.254}"

pp_hard_cgnat_compose() {
  pp_hard_need_cmd docker
  mkdir -p "${PP_HARD_CGNAT_SHARE_DIR}"
  if [[ "${PP_HARD_PROBE_DIR}" != /* ]]; then
    PP_HARD_PROBE_DIR="$(cd "${ROOT}/${PP_HARD_PROBE_DIR}" && pwd)"
  fi
  if [[ "${PP_HARD_CGNAT_SHARE_DIR}" != /* ]]; then
    mkdir -p "${PP_HARD_CGNAT_SHARE_DIR}"
    PP_HARD_CGNAT_SHARE_DIR="$(cd "${PP_HARD_CGNAT_SHARE_DIR}" && pwd)"
  fi
  PP_HARD_PROBE_DIR="${PP_HARD_PROBE_DIR}" PP_HARD_SHARE_DIR="${PP_HARD_CGNAT_SHARE_DIR}" \
    docker compose -p "${PP_HARD_CGNAT_COMPOSE_PROJECT}" -f "${PP_HARD_CGNAT_COMPOSE_FILE}" "$@"
}

pp_hard_cgnat_fix_peer_routes() {
  # Docker's default gw is the bridge, not our SNAT gateway — replace it.
  pp_hard_exec "${PP_HARD_CGNAT_PEER_A}" sh -c \
    "ip route del default 2>/dev/null || true; ip route replace default via ${PP_HARD_CGNAT_GW_A_PRIV_IP}"
  pp_hard_exec "${PP_HARD_CGNAT_PEER_B}" sh -c \
    "ip route del default 2>/dev/null || true; ip route replace default via ${PP_HARD_CGNAT_GW_B_PRIV_IP}"
  echo "ok  peer default routes via SNAT gateways"
}

pp_hard_cgnat_resolve_topology() {
  HOP_IP_PUBLIC="$(pp_hard_container_ip_on_net "${PP_HARD_CGNAT_HOP}" "${PP_HARD_CGNAT_NET_PUBLIC}")"
  PEER_A_IP="$(pp_hard_container_ip_on_net "${PP_HARD_CGNAT_PEER_A}" "${PP_HARD_CGNAT_NET_PRIV_A}")"
  PEER_B_IP="$(pp_hard_container_ip_on_net "${PP_HARD_CGNAT_PEER_B}" "${PP_HARD_CGNAT_NET_PRIV_B}")"
  [[ -n "${HOP_IP_PUBLIC}" ]] || pp_hard_die "cgnat hop missing public IP"
  [[ -n "${PEER_A_IP}" && -n "${PEER_B_IP}" ]] || pp_hard_die "cgnat peers missing private IPs"
  # Temporarily point status URL at cgnat hop for peer_id fetch.
  local saved_status="${PP_HARD_STATUS_URL}"
  PP_HARD_STATUS_URL="${PP_HARD_CGNAT_STATUS_URL}"
  HOP_PEER_ID="$(pp_hard_hop_peer_id)"
  PP_HARD_STATUS_URL="${saved_status}"
  HOP_MA_PUBLIC="$(pp_hard_hop_ma_for_ip "${HOP_IP_PUBLIC}" "${HOP_PEER_ID}")"
}

pp_hard_cgnat_assert_nat_shape() {
  echo "=== N-HARD-CGNAT-ISH topology asserts ==="

  # A cannot reach B private IP (isolated priv nets + no hairpin).
  if pp_hard_exec "${PP_HARD_CGNAT_PEER_A}" ping -c1 -W1 "${PEER_B_IP}" >/dev/null 2>&1; then
    pp_hard_die "peer-a unexpectedly reached peer-b private ${PEER_B_IP}"
  fi
  echo "ok  direct A→B private blocked"

  # Hop cannot ping peer private IPs (not on priv nets / no route).
  if pp_hard_exec "${PP_HARD_CGNAT_HOP}" ping -c1 -W1 "${PEER_A_IP}" >/dev/null 2>&1; then
    pp_hard_die "hop unexpectedly reached peer-a private ${PEER_A_IP}"
  fi
  if pp_hard_exec "${PP_HARD_CGNAT_HOP}" ping -c1 -W1 "${PEER_B_IP}" >/dev/null 2>&1; then
    pp_hard_die "hop unexpectedly reached peer-b private ${PEER_B_IP}"
  fi
  echo "ok  hop↛peer private (no inbound without mapping)"

  # Peers can reach hop public IP via SNAT gateways.
  pp_hard_exec "${PP_HARD_CGNAT_PEER_A}" ping -c1 -W2 "${HOP_IP_PUBLIC}" >/dev/null \
    || pp_hard_die "peer-a cannot ping hop public ${HOP_IP_PUBLIC} via SNAT"
  pp_hard_exec "${PP_HARD_CGNAT_PEER_B}" ping -c1 -W2 "${HOP_IP_PUBLIC}" >/dev/null \
    || pp_hard_die "peer-b cannot ping hop public ${HOP_IP_PUBLIC} via SNAT"
  echo "ok  A→hop and B→hop via SNAT"
}

pp_hard_cgnat_ensure_up() {
  local skip_up="${1:-0}"
  mkdir -p "${PP_HARD_CGNAT_SHARE_DIR}"
  if [[ "${skip_up}" -eq 0 ]]; then
    echo "=== hard-lab CGNAT compose up project=${PP_HARD_CGNAT_COMPOSE_PROJECT} ==="
    if [[ ! -f "${ROOT}/dist/pp-node/docker/Dockerfile" ]]; then
      pp_hard_die "missing dist/pp-node/docker; package with scripts/platform/pp_node_package_linux.sh all"
    fi
    pp_hard_cgnat_compose up -d --build --force-recreate
  fi
  echo "=== wait cgnat hop healthz ${PP_HARD_CGNAT_STATUS_URL} ==="
  for _ in $(seq 1 60); do
    if curl -fsS -m 2 "${PP_HARD_CGNAT_STATUS_URL}/healthz" >/dev/null 2>&1; then
      break
    fi
    sleep 0.5
  done
  local saved_status="${PP_HARD_STATUS_URL}"
  PP_HARD_STATUS_URL="${PP_HARD_CGNAT_STATUS_URL}"
  pp_hard_wait_healthz
  PP_HARD_STATUS_URL="${saved_status}"

  pp_hard_cgnat_fix_peer_routes
  # Give gw entrypoints a moment to install iptables.
  sleep 1
  pp_hard_cgnat_resolve_topology
  echo "cgnat hop peer_id=${HOP_PEER_ID} public=${HOP_IP_PUBLIC}"
  echo "cgnat peer-a=${PEER_A_IP} peer-b=${PEER_B_IP}"
  echo "cgnat hop_ma=${HOP_MA_PUBLIC}"
}
