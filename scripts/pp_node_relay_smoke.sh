#!/usr/bin/env bash
# Run L0 HTTP smoke, then L1 Amp relay probe if pp-node-probe is available.
#
# Prerequisites:
#   - pp-node listening (e.g. docker compose -f packaging/pp-node/docker-compose.yml up -d)
#   - status HTTP published (PP_NODE_STATUS_ADDR=0.0.0.0:18518)
#   - for L1: build/src/app/node/pp-node-probe (cmake --build build --target pp-node-probe)
#
# See packaging/pp-node/IMAGE_SMOKE.md
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=pp_node_hop_lib.sh
source "${ROOT}/scripts/pp_node_hop_lib.sh"

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

hop="$(pp_node_hop_multiaddr "${STATUS_URL}")"
advertise_host="$(pp_node_advertise_host)"
echo "L1 hop multiaddr: ${hop}"
if [[ "${advertise_host}" == "127.0.0.1" && -z "${PP_NODE_PROBE_ADVERTISE_HOST:-}" ]]; then
  echo "warn: using advertise-host=127.0.0.1 (circuit bridge fails if hop is in Docker)"
else
  echo "L1 advertise-host (for circuit target): ${advertise_host}"
fi

PP_NODE_PROBE_HOP="${hop}" PP_NODE_PROBE_ADVERTISE_HOST="${advertise_host}" \
  "${PROBE_BIN}" --hop "${hop}" --advertise-host "${advertise_host}"
echo "pp-node L0+L1 smoke PASSED"
