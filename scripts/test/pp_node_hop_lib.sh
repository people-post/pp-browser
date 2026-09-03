# Shared hop multiaddr / advertise-host helpers for pp-node smokes.
# Source from other scripts after STATUS_URL is set.
# shellcheck shell=bash

pp_node_need_cmd() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "error: missing command: $1" >&2
    exit 1
  }
}

pp_node_status_auth_args() {
  if [[ -n "${PP_NODE_STATUS_TOKEN:-}" ]]; then
    printf '%s\n' -H "Authorization: Bearer ${PP_NODE_STATUS_TOKEN}"
  fi
}

pp_node_hop_multiaddr() {
  local status_url="${1:-${PP_NODE_STATUS_URL:-http://127.0.0.1:18518}}"
  pp_node_need_cmd curl
  pp_node_need_cmd python3
  local auth_args=()
  if [[ -n "${PP_NODE_STATUS_TOKEN:-}" ]]; then
    auth_args=(-H "Authorization: Bearer ${PP_NODE_STATUS_TOKEN}")
  fi
  local status_json
  status_json="$(curl -fsS -m 5 "${auth_args[@]}" "${status_url}/status")"
  STATUS_JSON="${status_json}" python3 <<'PY'
import json, os
s = json.loads(os.environ["STATUS_JSON"])
peer = s.get("peer_id") or ""
listen = s.get("listen") or ""
if not peer or not listen:
    raise SystemExit("missing peer_id or listen in /status")
if "/udp/" not in listen or "/adp/" not in listen:
    raise SystemExit(f"listen is not Amp ADP (/udp/…/adp/…): {listen}")
listen = listen.replace("/ip4/0.0.0.0/", "/ip4/127.0.0.1/")
listen = listen.replace("/ip6/::/", "/ip6/::1/")
if "/p2p/" in listen:
    print(listen)
else:
    print(f"{listen}/p2p/{peer}")
PY
}

pp_node_advertise_host() {
  local advertise_host="${PP_NODE_PROBE_ADVERTISE_HOST:-}"
  if [[ -z "${advertise_host}" ]]; then
    if [[ -d /sys/class/net/docker0 ]]; then
      advertise_host="$(ip -4 -o addr show docker0 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | head -1 || true)"
    fi
  fi
  if [[ -z "${advertise_host}" ]]; then
    advertise_host="$(docker network inspect bridge --format '{{(index .IPAM.Config 0).Gateway}}' 2>/dev/null || true)"
  fi
  if [[ -z "${advertise_host}" ]]; then
    advertise_host=127.0.0.1
  fi
  printf '%s\n' "${advertise_host}"
}

pp_node_hop_stats() {
  local container="${1:-${PP_LOCAL_HOP_CONTAINER:-pp-node-relay-smoke-hop}}"
  if ! command -v docker >/dev/null 2>&1; then
    echo "hop_stats skipped (no docker)"
    return 0
  fi
  if ! docker inspect "${container}" >/dev/null 2>&1; then
    echo "hop_stats skipped (no container ${container})"
    return 0
  fi
  docker stats --no-stream --format \
    'hop_stats container={{.Name}} mem={{.MemUsage}} pids={{.PIDs}}' \
    "${container}" || true
}
