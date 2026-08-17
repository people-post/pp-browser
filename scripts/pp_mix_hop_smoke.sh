#!/usr/bin/env bash
# N-MIX: run existing hop smokes in parallel against one live pp-node (interference).
# Allowlist: call-hop (2 cycles) ∥ N-FANOUT ∥ circuit-cap M=2. Combined load stays
# under N₀=8. Excludes chaos, cap sweep, soak.
# Each child keeps its own pass/fail; mix also requires /healthz afterward.
# See docs/ops/TEST_STRATEGY.md
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=pp_node_hop_lib.sh
source "${ROOT}/scripts/pp_node_hop_lib.sh"
# shellcheck source=pp_mix_lib.sh
source "${ROOT}/scripts/pp_mix_lib.sh"

STATUS_URL="${PP_NODE_STATUS_URL:-http://127.0.0.1:18518}"
PROBE_BIN="${PP_NODE_PROBE_BIN:-${ROOT}/build/src/app/node/pp-node-probe}"
CALL_PROBE_BIN="${PP_CALL_PROBE_BIN:-${ROOT}/build/src/app/node/pp-call-probe}"
BRIDGES="${PP_MIX_BRIDGES:-2}"
CYCLES="${PP_MIX_CALL_HOP_CYCLES:-2}"
HOP_CONTAINER="${PP_LOCAL_HOP_CONTAINER:-pp-node-relay-smoke-hop}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --status-url) STATUS_URL="$2"; shift 2 ;;
    --probe-bin) PROBE_BIN="$2"; shift 2 ;;
    --call-probe-bin) CALL_PROBE_BIN="$2"; shift 2 ;;
    --bridges) BRIDGES="$2"; shift 2 ;;
    --cycles) CYCLES="$2"; shift 2 ;;
    -h|--help)
      echo "Usage: $(basename "$0") [--status-url URL] [--probe-bin PATH] [--call-probe-bin PATH]"
      echo "                        [--bridges M] [--cycles K]"
      exit 0
      ;;
    *) echo "error: unknown arg: $1" >&2; exit 2 ;;
  esac
done

export PP_NODE_STATUS_URL="${STATUS_URL}"
bash "${ROOT}/scripts/pp_node_image_smoke.sh" --status-url "${STATUS_URL}"

if [[ ! -x "${PROBE_BIN}" ]]; then
  echo "error: pp-node-probe missing (${PROBE_BIN})" >&2
  exit 1
fi
if [[ ! -x "${CALL_PROBE_BIN}" ]]; then
  echo "error: pp-call-probe missing (${CALL_PROBE_BIN})" >&2
  exit 1
fi

export PP_NODE_PROBE_BIN="${PROBE_BIN}"
export PP_CALL_PROBE_BIN="${CALL_PROBE_BIN}"
export PP_CALL_PROBE_CYCLES="${CYCLES}"
export PP_NODE_PROBE_BRIDGES="${BRIDGES}"

echo "=== N-MIX hop interference (call-hop×${CYCLES} ∥ fanout ∥ circuit-cap M=${BRIDGES}) ==="
pp_mix_run_parallel \
  call-hop "bash '${ROOT}/scripts/pp_call_hop_smoke.sh' --status-url '${STATUS_URL}' --probe-bin '${CALL_PROBE_BIN}' --cycles '${CYCLES}'" \
  fanout "bash '${ROOT}/scripts/pp_node_fanout_smoke.sh' --status-url '${STATUS_URL}' --probe-bin '${PROBE_BIN}'" \
  circuit-cap "bash '${ROOT}/scripts/pp_node_circuit_cap_smoke.sh' --status-url '${STATUS_URL}' --probe-bin '${PROBE_BIN}' --bridges '${BRIDGES}'"

if ! curl -fsS -m 5 "${STATUS_URL}/healthz" >/dev/null; then
  echo "error: hop /healthz failed after mix" >&2
  exit 1
fi
echo "ok  hop /healthz after mix"
pp_node_hop_stats "${HOP_CONTAINER}"
echo "pp-mix-hop smoke PASSED"
