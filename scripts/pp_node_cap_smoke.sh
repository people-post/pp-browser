#!/usr/bin/env bash
# L0 + N-CAP-MEDIA capacity curve against a live pp-node hop.
#
# Default (cheap, used by --suite node): N=4, 100% attach.
# Sweep (--suite cap / PP_NODE_CAP_SWEEP): sequential N, p50/p95, hop RSS/FD.
# Soft SLO: N<=8 require 100% attach; larger N informational unless hop dies.
#
# See packaging/pp-node/IMAGE_SMOKE.md and docs/ops/TEST_STRATEGY.md (N-CAP-MEDIA)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=pp_node_hop_lib.sh
source "${ROOT}/scripts/pp_node_hop_lib.sh"

STATUS_URL="${PP_NODE_STATUS_URL:-http://127.0.0.1:18518}"
PROBE_BIN="${PP_NODE_PROBE_BIN:-${ROOT}/build/src/app/node/pp-node-probe}"
ATTACHERS="${PP_NODE_PROBE_ATTACHERS:-4}"
SWEEP="${PP_NODE_CAP_SWEEP:-}"
HOP_CONTAINER="${PP_LOCAL_HOP_CONTAINER:-pp-node-relay-smoke-hop}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --status-url) STATUS_URL="$2"; shift 2 ;;
    --probe-bin) PROBE_BIN="$2"; shift 2 ;;
    --attachers) ATTACHERS="$2"; shift 2 ;;
    --sweep) SWEEP="$2"; shift 2 ;;
    -h|--help)
      echo "Usage: $(basename "$0") [--status-url URL] [--probe-bin PATH] [--attachers N] [--sweep 4,8,12,16|4:16:4]"
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
spec="${SWEEP:-${ATTACHERS}}"
echo "N-CAP-MEDIA hop multiaddr: ${hop} attachers=${spec}"
PP_NODE_PROBE_HOP="${hop}" "${PROBE_BIN}" --hop "${hop}" --mode media-cap --attachers "${spec}"
pp_node_hop_stats "${HOP_CONTAINER}"
echo "pp-node L0+N-CAP-MEDIA smoke PASSED"
