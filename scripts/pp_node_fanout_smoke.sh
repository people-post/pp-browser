#!/usr/bin/env bash
# L0 + L2 N-FANOUT (media attach×2 + frame) against a live pp-node hop.
#
# Prerequisites:
#   - pp-node listening (docker compose -f packaging/pp-node/docker-compose.yml up -d
#     or docker compose -f packaging/pp-node/docker-compose.relay-smoke.yml up -d)
#   - status HTTP published (PP_NODE_STATUS_ADDR=0.0.0.0:18518)
#   - build/src/app/node/pp-node-probe (cmake --build build --target pp-node-probe)
#
# See packaging/pp-node/IMAGE_SMOKE.md and docs/ops/TEST_STRATEGY.md (N-FANOUT)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STATUS_URL="${PP_NODE_STATUS_URL:-http://127.0.0.1:18518}"
PROBE_BIN="${PP_NODE_PROBE_BIN:-${ROOT}/build/src/app/node/pp-node-probe}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --status-url) STATUS_URL="$2"; shift 2 ;;
    --probe-bin) PROBE_BIN="$2"; shift 2 ;;
    -h|--help)
      echo "Usage: $(basename "$0") [--status-url URL] [--probe-bin PATH]"
      exit 0
      ;;
    *) echo "error: unknown arg: $1" >&2; exit 2 ;;
  esac
done

export PP_NODE_STATUS_URL="${STATUS_URL}"
bash "${ROOT}/scripts/pp_node_image_smoke.sh"

if [[ ! -x "${PROBE_BIN}" ]]; then
  echo "error: N-FANOUT probe binary missing (${PROBE_BIN}); build with:" >&2
  echo "  cmake --build build --target pp-node-probe" >&2
  exit 1
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
import json, os
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

echo "N-FANOUT hop multiaddr: ${hop}"
PP_NODE_PROBE_HOP="${hop}" "${PROBE_BIN}" --hop "${hop}" --mode media-fanout
echo "pp-node L0+N-FANOUT smoke PASSED"
