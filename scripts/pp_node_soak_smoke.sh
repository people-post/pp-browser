#!/usr/bin/env bash
# N-SOAK: attach/detach/fan-out churn against a live hop.
#
# Default duration 120s (CI/local). Weekly: PP_NODE_SOAK_SEC=3600.
# See docs/ops/TEST_STRATEGY.md (N-SOAK)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=pp_node_hop_lib.sh
source "${ROOT}/scripts/pp_node_hop_lib.sh"

STATUS_URL="${PP_NODE_STATUS_URL:-http://127.0.0.1:18518}"
PROBE_BIN="${PP_NODE_PROBE_BIN:-${ROOT}/build/src/app/node/pp-node-probe}"
DURATION="${PP_NODE_SOAK_SEC:-120}"
CHURN="${PP_NODE_PROBE_CHURN:-4}"
HOP_CONTAINER="${PP_LOCAL_HOP_CONTAINER:-pp-node-relay-smoke-hop}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --status-url) STATUS_URL="$2"; shift 2 ;;
    --probe-bin) PROBE_BIN="$2"; shift 2 ;;
    --duration) DURATION="$2"; shift 2 ;;
    --churn) CHURN="$2"; shift 2 ;;
    -h|--help)
      echo "Usage: $(basename "$0") [--status-url URL] [--probe-bin PATH] [--duration SEC] [--churn N]"
      echo "Env: PP_NODE_SOAK_SEC (default 120; weekly 3600) PP_NODE_PROBE_CHURN (default 4)"
      exit 0
      ;;
    *) echo "error: unknown arg: $1" >&2; exit 2 ;;
  esac
done

export PP_NODE_STATUS_URL="${STATUS_URL}"
bash "${ROOT}/scripts/pp_node_image_smoke.sh"

if [[ ! -x "${PROBE_BIN}" ]]; then
  echo "error: probe binary missing (${PROBE_BIN}); build with:" >&2
  echo "  cmake --build build --target pp-node-probe" >&2
  exit 1
fi

hop="$(pp_node_hop_multiaddr "${STATUS_URL}")"
echo "N-SOAK hop multiaddr: ${hop} duration=${DURATION}s churn=${CHURN}"
PP_NODE_PROBE_HOP="${hop}" PP_NODE_SOAK_SEC="${DURATION}" \
  "${PROBE_BIN}" --hop "${hop}" --mode media-soak --duration "${DURATION}" --churn "${CHURN}"
pp_node_hop_stats "${HOP_CONTAINER}"
echo "pp-node N-SOAK smoke PASSED"
