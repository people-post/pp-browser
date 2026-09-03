#!/usr/bin/env bash
# L0 + N-CAP-CIRCUIT: M concurrent bridges through a live hop to local targets.
#
# Soft SLO: M<=4 require 100% payload round-trip; larger M informational.
# See docs/ops/TEST_STRATEGY.md (N-CAP-CIRCUIT)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=pp_node_hop_lib.sh
source "${ROOT}/scripts/test/pp_node_hop_lib.sh"

STATUS_URL="${PP_NODE_STATUS_URL:-http://127.0.0.1:18518}"
PROBE_BIN="${PP_NODE_PROBE_BIN:-${ROOT}/build/src/app/node/pp-node-probe}"
BRIDGES="${PP_NODE_PROBE_BRIDGES:-4}"
HOP_CONTAINER="${PP_LOCAL_HOP_CONTAINER:-pp-node-relay-smoke-hop}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --status-url) STATUS_URL="$2"; shift 2 ;;
    --probe-bin) PROBE_BIN="$2"; shift 2 ;;
    --bridges) BRIDGES="$2"; shift 2 ;;
    -h|--help)
      echo "Usage: $(basename "$0") [--status-url URL] [--probe-bin PATH] [--bridges M|M,M,...]"
      exit 0
      ;;
    *) echo "error: unknown arg: $1" >&2; exit 2 ;;
  esac
done

export PP_NODE_STATUS_URL="${STATUS_URL}"
bash "${ROOT}/scripts/test/pp_node_image_smoke.sh"

if [[ ! -x "${PROBE_BIN}" ]]; then
  echo "error: probe binary missing (${PROBE_BIN}); build with:" >&2
  echo "  cmake --build build --target pp-node-probe" >&2
  exit 1
fi

hop="$(pp_node_hop_multiaddr "${STATUS_URL}")"
advertise_host="$(pp_node_advertise_host)"
if [[ "${advertise_host}" == "127.0.0.1" ]]; then
  echo "warn: using advertise-host=127.0.0.1 (circuit bridge fails if hop is in Docker)"
else
  echo "N-CAP-CIRCUIT advertise-host: ${advertise_host}"
fi
echo "N-CAP-CIRCUIT hop multiaddr: ${hop} bridges=${BRIDGES}"
PP_NODE_PROBE_HOP="${hop}" PP_NODE_PROBE_ADVERTISE_HOST="${advertise_host}" \
  "${PROBE_BIN}" --hop "${hop}" --mode circuit-cap --advertise-host "${advertise_host}" \
  --bridges "${BRIDGES}"
pp_node_hop_stats "${HOP_CONTAINER}"
echo "pp-node L0+N-CAP-CIRCUIT smoke PASSED"
