#!/usr/bin/env bash
# Run L0 HTTP smoke, then L1 libp2p relay probe if pp-node-probe is available.
#
# Prerequisites:
#   - pp-node listening (e.g. docker compose -f packaging/pp-node/docker-compose.yml up -d)
#   - status HTTP published (PP_NODE_STATUS_ADDR=0.0.0.0:18518)
#   - for L1: build/src/app/node/pp-node-probe (cmake --build build --target pp-node-probe)
#
# See packaging/pp-node/IMAGE_SMOKE.md
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STATUS_URL="${PP_NODE_STATUS_URL:-http://127.0.0.1:18518}"
PROBE_BIN="${PP_NODE_PROBE_BIN:-${ROOT}/build/src/app/node/pp-node-probe}"
SKIP_L1=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --status-url) STATUS_URL="$2"; shift 2 ;;
    --probe-bin) PROBE_BIN="$2"; shift 2 ;;
    --l0-only) SKIP_L1=1; shift ;;
    -h|--help)
      echo "Usage: $(basename "$0") [--status-url URL] [--probe-bin PATH] [--l0-only]"
      exit 0
      ;;
    *) echo "error: unknown arg: $1" >&2; exit 2 ;;
  esac
done

export PP_NODE_STATUS_URL="${STATUS_URL}"
bash "${ROOT}/scripts/pp_node_image_smoke.sh"

if [[ "${SKIP_L1}" -eq 1 ]]; then
  echo "skipping L1 (--l0-only)"
  exit 0
fi

if [[ ! -x "${PROBE_BIN}" ]]; then
  echo "warn: L1 probe binary missing (${PROBE_BIN}); build with:"
  echo "  cmake --build build --target pp-node-probe"
  echo "L0-only PASSED (L1 skipped)"
  exit 0
fi

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "error: missing command: $1" >&2
    exit 1
  }
}
need_cmd curl
need_cmd python3

auth_args=()
if [[ -n "${PP_NODE_STATUS_TOKEN:-}" ]]; then
  auth_args=(-H "Authorization: Bearer ${PP_NODE_STATUS_TOKEN}")
fi

status_json="$(curl -fsS -m 5 "${auth_args[@]}" "${STATUS_URL}/status")"
hop="$(STATUS_JSON="${status_json}" python3 <<'PY'
import json, os, re
s = json.loads(os.environ["STATUS_JSON"])
peer = s.get("peer_id") or ""
listen = s.get("listen") or ""
if not peer or not listen:
    raise SystemExit("missing peer_id or listen in /status")
listen = listen.replace("/ip4/0.0.0.0/", "/ip4/127.0.0.1/")
listen = listen.replace("/ip6/::/", "/ip6/::1/")
if "/p2p/" in listen:
    print(listen)
else:
    print(f"{listen}/p2p/{peer}")
PY
)"

echo "L1 hop multiaddr: ${hop}"

# Circuit bridge: hop (often in Docker) must dial back to this host's target.
# Prefer explicit PP_NODE_PROBE_ADVERTISE_HOST; else docker0 / default bridge IP.
advertise_host="${PP_NODE_PROBE_ADVERTISE_HOST:-}"
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
  echo "warn: using advertise-host=127.0.0.1 (circuit bridge fails if hop is in Docker)"
else
  echo "L1 advertise-host (for circuit target): ${advertise_host}"
fi

PP_NODE_PROBE_HOP="${hop}" PP_NODE_PROBE_ADVERTISE_HOST="${advertise_host}" \
  "${PROBE_BIN}" --hop "${hop}" --advertise-host "${advertise_host}"
echo "pp-node L0+L1 smoke PASSED"
