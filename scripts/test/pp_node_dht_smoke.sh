#!/usr/bin/env bash
# Lab smoke: two local pp-node processes with DHT on discover each other's ADP addrs
# without Brief HTTP directory (n2-core acceptance).
#
# Prerequisites:
#   cmake --build build --target pp-node -j
#
# Usage:
#   ./scripts/test/pp_node_dht_smoke.sh
#   PP_NODE_BIN=build/src/app/node/pp-node ./scripts/test/pp_node_dht_smoke.sh
#
# See projects/p2p-mesh/DISCOVERY_ROADMAP.md (n2-core lab) and packaging/pp-node/IMAGE_SMOKE.md
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NODE_BIN="${PP_NODE_BIN:-${ROOT}/build/src/app/node/pp-node}"
PIN="${PP_BROWSER_PIN:-local-test-pin}"
TIMEOUT_SEC="${PP_NODE_DHT_SMOKE_TIMEOUT_SEC:-45}"
PORT_A="${PP_NODE_DHT_SMOKE_PORT_A:-19117}"
PORT_B="${PP_NODE_DHT_SMOKE_PORT_B:-19127}"
STATUS_A="${PP_NODE_DHT_SMOKE_STATUS_A:-127.0.0.1:19118}"
STATUS_B="${PP_NODE_DHT_SMOKE_STATUS_B:-127.0.0.1:19128}"
# Distinct ≥32-byte hex seeds (fail-closed identity mint for empty volumes).
SEED_A="${PP_NODE_DHT_SMOKE_SEED_A:-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa}"
SEED_B="${PP_NODE_DHT_SMOKE_SEED_B:-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb}"

WORKDIR="${PP_NODE_DHT_SMOKE_WORKDIR:-$(mktemp -d -t pp-node-dht-smoke.XXXXXX)}"
CLEANUP_WORKDIR=1
if [[ -n "${PP_NODE_DHT_SMOKE_WORKDIR:-}" ]]; then
  CLEANUP_WORKDIR=0
  mkdir -p "${WORKDIR}"
fi

PID_A=""
PID_B=""

cleanup() {
  if [[ -n "${PID_B}" ]] && kill -0 "${PID_B}" 2>/dev/null; then
    kill "${PID_B}" 2>/dev/null || true
    wait "${PID_B}" 2>/dev/null || true
  fi
  if [[ -n "${PID_A}" ]] && kill -0 "${PID_A}" 2>/dev/null; then
    kill "${PID_A}" 2>/dev/null || true
    wait "${PID_A}" 2>/dev/null || true
  fi
  if [[ "${CLEANUP_WORKDIR}" -eq 1 ]]; then
    rm -rf "${WORKDIR}"
  fi
}
trap cleanup EXIT

die() {
  echo "error: $*" >&2
  exit 1
}

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "missing command: $1"
}

curl_status() {
  local url="$1"
  curl -fsS --max-time 2 "${url}/status" 2>/dev/null || true
}

wait_health() {
  local url="$1"
  local label="$2"
  local deadline=$((SECONDS + TIMEOUT_SEC))
  while (( SECONDS < deadline )); do
    if curl -fsS --max-time 2 "${url}/healthz" >/dev/null 2>&1; then
      echo "${label} healthy at ${url}"
      return 0
    fi
    sleep 0.25
  done
  die "${label} did not become healthy within ${TIMEOUT_SEC}s (${url})"
}

# Returns 0 when status JSON for $1 contains a DHT record for peer_id $2 with a non-empty multiaddr.
has_peer_record() {
  local json="$1"
  local want_peer="$2"
  python3 - "$json" "$want_peer" <<'PY'
import json, sys
raw, want = sys.argv[1], sys.argv[2]
try:
    doc = json.loads(raw)
except Exception:
    sys.exit(1)
stats = doc.get("dht_stats") or {}
for row in stats.get("records") or []:
    if row.get("peer_id") != want:
        continue
    addrs = row.get("multiaddrs") or []
    if any(isinstance(a, str) and "/adp/" in a for a in addrs):
        sys.exit(0)
sys.exit(1)
PY
}

need_cmd curl
need_cmd python3
need_cmd jq

[[ -x "${NODE_BIN}" ]] || die "pp-node missing (${NODE_BIN}); build with: cmake --build build --target pp-node"

DIR_A="${WORKDIR}/node-a"
DIR_B="${WORKDIR}/node-b"
mkdir -p "${DIR_A}" "${DIR_B}"
LOG_A="${WORKDIR}/node-a.log"
LOG_B="${WORKDIR}/node-b.log"

echo "DHT lab workdir: ${WORKDIR}"
echo "starting node A (UDP ${PORT_A}, status ${STATUS_A})"

PP_BROWSER_PIN="${PIN}" \
PP_NODE_DATA_DIR="${DIR_A}" \
PP_NODE_AMP_UDP_PORT="${PORT_A}" \
PP_NODE_STATUS_ADDR="${STATUS_A}" \
PP_NODE_CAP_DHT=1 \
PP_NODE_CAP_CIRCUIT_RELAY=0 \
PP_NODE_CAP_MEDIA_RELAY=0 \
PP_NODE_IDENTITY_SEED="${SEED_A}" \
PP_NODE_MESH_PUBLISH=0 \
  "${NODE_BIN}" >"${LOG_A}" 2>&1 &
PID_A=$!

URL_A="http://${STATUS_A}"
wait_health "${URL_A}" "node-A"

STATUS_JSON_A="$(curl_status "${URL_A}")"
[[ -n "${STATUS_JSON_A}" ]] || die "empty /status from node A"
echo "${STATUS_JSON_A}" | jq -e '.dht == true and .dht_stats.participate == true' >/dev/null \
  || die "node A DHT not participating: ${STATUS_JSON_A}"

PEER_A="$(echo "${STATUS_JSON_A}" | jq -r '.peer_id')"
LISTEN_A="$(echo "${STATUS_JSON_A}" | jq -r '.listen')"
[[ -n "${PEER_A}" && "${PEER_A}" != "null" ]] || die "node A missing peer_id"
[[ -n "${LISTEN_A}" && "${LISTEN_A}" != "null" ]] || die "node A missing listen"

# Replace 0.0.0.0 with loopback so B can dial A on the host.
BOOTSTRAP_A="${LISTEN_A/0.0.0.0/127.0.0.1}"
echo "node A peer=${PEER_A} bootstrap=${BOOTSTRAP_A}"

echo "starting node B (UDP ${PORT_B}, status ${STATUS_B}, bootstrap=A)"
PP_BROWSER_PIN="${PIN}" \
PP_NODE_DATA_DIR="${DIR_B}" \
PP_NODE_AMP_UDP_PORT="${PORT_B}" \
PP_NODE_STATUS_ADDR="${STATUS_B}" \
PP_NODE_CAP_DHT=1 \
PP_NODE_CAP_CIRCUIT_RELAY=0 \
PP_NODE_CAP_MEDIA_RELAY=0 \
PP_NODE_IDENTITY_SEED="${SEED_B}" \
PP_NODE_BOOTSTRAP_PEERS="${BOOTSTRAP_A}" \
PP_NODE_MESH_PUBLISH=0 \
  "${NODE_BIN}" >"${LOG_B}" 2>&1 &
PID_B=$!

URL_B="http://${STATUS_B}"
wait_health "${URL_B}" "node-B"

STATUS_JSON_B="$(curl_status "${URL_B}")"
PEER_B="$(echo "${STATUS_JSON_B}" | jq -r '.peer_id')"
[[ -n "${PEER_B}" && "${PEER_B}" != "null" ]] || die "node B missing peer_id"
echo "node B peer=${PEER_B}"

deadline=$((SECONDS + TIMEOUT_SEC))
got_a=0
got_b=0
while (( SECONDS < deadline )); do
  STATUS_JSON_A="$(curl_status "${URL_A}")"
  STATUS_JSON_B="$(curl_status "${URL_B}")"
  if has_peer_record "${STATUS_JSON_A}" "${PEER_B}"; then
    got_a=1
  fi
  if has_peer_record "${STATUS_JSON_B}" "${PEER_A}"; then
    got_b=1
  fi
  if [[ "${got_a}" -eq 1 && "${got_b}" -eq 1 ]]; then
    echo "mutual DHT records present"
    echo "  A has B: $(echo "${STATUS_JSON_A}" | jq -c '.dht_stats.records')"
    echo "  B has A: $(echo "${STATUS_JSON_B}" | jq -c '.dht_stats.records')"
    echo "pp-node DHT lab smoke PASSED"
    exit 0
  fi
  sleep 0.5
done

echo "--- node A log (tail) ---" >&2
tail -n 40 "${LOG_A}" >&2 || true
echo "--- node B log (tail) ---" >&2
tail -n 40 "${LOG_B}" >&2 || true
die "timed out waiting for mutual DHT discovery (A_has_B=${got_a} B_has_A=${got_b})"
