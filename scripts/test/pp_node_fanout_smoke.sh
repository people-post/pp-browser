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
# shellcheck source=pp_node_hop_lib.sh
source "${ROOT}/scripts/pp_node_hop_lib.sh"

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

hop="$(pp_node_hop_multiaddr "${STATUS_URL}")"
echo "N-FANOUT hop multiaddr: ${hop}"
PP_NODE_PROBE_HOP="${hop}" "${PROBE_BIN}" --hop "${hop}" --mode media-fanout
echo "pp-node L0+N-FANOUT smoke PASSED"
