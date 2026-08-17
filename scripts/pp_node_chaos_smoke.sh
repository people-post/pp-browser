#!/usr/bin/env bash
# N-CHAOS: kill client mid-attach; docker restart hop; optional pause/unpause.
# Soft pass: hop recovers and accepts new work. In-flight streams need not survive restart.
#
# See docs/ops/TEST_STRATEGY.md (N-CHAOS)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=pp_node_hop_lib.sh
source "${ROOT}/scripts/pp_node_hop_lib.sh"

STATUS_URL="${PP_NODE_STATUS_URL:-http://127.0.0.1:18518}"
PROBE_BIN="${PP_NODE_PROBE_BIN:-${ROOT}/build/src/app/node/pp-node-probe}"
HOP_CONTAINER="${PP_LOCAL_HOP_CONTAINER:-pp-node-relay-smoke-hop}"
PAUSE_SEC="${PP_NODE_CHAOS_PAUSE_SEC:-10}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --status-url) STATUS_URL="$2"; shift 2 ;;
    --probe-bin) PROBE_BIN="$2"; shift 2 ;;
    --pause-sec) PAUSE_SEC="$2"; shift 2 ;;
    -h|--help)
      echo "Usage: $(basename "$0") [--status-url URL] [--probe-bin PATH] [--pause-sec N]"
      exit 0
      ;;
    *) echo "error: unknown arg: $1" >&2; exit 2 ;;
  esac
done

export PP_NODE_STATUS_URL="${STATUS_URL}"
pp_node_need_cmd docker

if [[ ! -x "${PROBE_BIN}" ]]; then
  echo "error: probe binary missing (${PROBE_BIN}); build with:" >&2
  echo "  cmake --build build --target pp-node-probe" >&2
  exit 1
fi

bash "${ROOT}/scripts/pp_node_image_smoke.sh"
hop="$(pp_node_hop_multiaddr "${STATUS_URL}")"

echo "=== N-CHAOS 1: kill client mid-attach, then hop accepts N=2 ==="
set +e
PP_NODE_PROBE_HOP="${hop}" timeout 1s "${PROBE_BIN}" --hop "${hop}" --mode media-cap --attachers 4
kill_rc=$?
set -e
# timeout(1) returns 124 on kill; probe may also exit 1/143. All are expected.
echo "killed mid-attach rc=${kill_rc} (expected non-zero)"
PP_NODE_PROBE_HOP="${hop}" "${PROBE_BIN}" --hop "${hop}" --mode media-cap --attachers 2
echo "ok  hop accepted new attach after client kill"

echo "=== N-CHAOS 2: docker restart hop, then L0+L1 ==="
docker restart "${HOP_CONTAINER}"
export PP_NODE_STATUS_URL="${STATUS_URL}"
# Wait for /healthz after restart (image smoke retries).
bash "${ROOT}/scripts/pp_node_image_smoke.sh" --status-url "${STATUS_URL}"
bash "${ROOT}/scripts/pp_node_relay_smoke.sh" --status-url "${STATUS_URL}"
echo "ok  L0+L1 after docker restart"

echo "=== N-CHAOS 3: docker pause ${PAUSE_SEC}s / unpause, then hop accepts N=2 ==="
docker pause "${HOP_CONTAINER}"
sleep "${PAUSE_SEC}"
docker unpause "${HOP_CONTAINER}"
bash "${ROOT}/scripts/pp_node_image_smoke.sh" --status-url "${STATUS_URL}"
hop="$(pp_node_hop_multiaddr "${STATUS_URL}")"
PP_NODE_PROBE_HOP="${hop}" "${PROBE_BIN}" --hop "${hop}" --mode media-cap --attachers 2
echo "ok  hop accepted new attach after pause/unpause"

pp_node_hop_stats "${HOP_CONTAINER}"
echo "pp-node N-CHAOS smoke PASSED"
