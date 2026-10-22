#!/usr/bin/env bash
# DHT lab (N-DHT): an org seed and 12 DHT Nodes (k = 3) on an internal network. Once the routing
# tables fill, the seed stops; every Node must then find every other Node's record by lookup.
# N-DHT-CALL: a product-stack callee publishes while the seed is up; with the seed down the caller
# finds it through the DHT and calls it at that address only (it is handed a dead one).
# Design: pp-cpp-mesh docs/DHT.md § 5. Compose: packaging/pp-node/docker-compose.dht-lab.yml.
#
# Usage: pp_dht_lab_smoke.sh [--keep]   (pp_local_test.sh --suite dht stages pp-node:local first)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
COMPOSE_FILE="${ROOT}/packaging/pp-node/docker-compose.dht-lab.yml"
NODES="${PP_DHT_LAB_NODES:-12}"
K="${PP_DHT_LAB_K:-3}"
SHARE_DIR="${PP_DHT_LAB_SHARE_DIR:-/tmp/pp-dht-lab-share}"
PROBE_DIR="${PP_DHT_LAB_PROBE_DIR:-${ROOT}/build/src/app/node}"
export PP_DHT_LAB_SHARE_DIR="${SHARE_DIR}" PP_DHT_LAB_PROBE_DIR="${PROBE_DIR}"
[[ -x "${PROBE_DIR}/pp-call-probe" ]] || { echo "error: ${PROBE_DIR}/pp-call-probe missing" >&2; exit 1; }
KEEP=0
[[ "${1:-}" == "--keep" ]] && KEEP=1

compose() { docker compose -f "${COMPOSE_FILE}" "$@"; }
ctl() { docker exec pp-dht-lab-ctl python3 /scripts/pp_dht_lab.py "$@" --nodes "${NODES}"; }

callee_pid=""
cleanup() {
  [[ -n "${callee_pid}" ]] && { kill "${callee_pid}" 2>/dev/null || true; wait "${callee_pid}" 2>/dev/null || true; }
  if [[ "${KEEP}" -eq 0 ]]; then
    compose down -v --remove-orphans >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

dht_services=()
for i in $(seq 1 "${NODES}"); do dht_services+=("dht-${i}"); done

echo "=== N-DHT: seed + ${NODES} DHT Nodes (k=${K}) ==="
compose down -v --remove-orphans >/dev/null 2>&1 || true
mkdir -p "${SHARE_DIR}"
compose up -d --build seed ctl callee caller
docker exec pp-dht-lab-callee sh -c 'rm -rf /share/*'  # files from an earlier run are root's
SEED_MA="$(ctl seed-ma --timeout 60)"
echo "seed: ${SEED_MA}"

PP_DHT_SEED_MA="${SEED_MA}" compose up -d "${dht_services[@]}"
ctl wait-tables --min-contacts 3 --timeout 120

# The callee publishes its record while the seed is up, then waits for a call.
call_id="pp-dht-lab-call"
docker exec pp-dht-lab-callee /probes/pp-call-probe --role answerer --product-stack \
  --listen /ip4/0.0.0.0/udp/18600/adp/1.0.0 --advertise-host 10.78.21.10 \
  --ready-file /share/callee.ready --signal-dir /share/sig --call-id "${call_id}" --hold-seconds 600 \
  --dht "${SEED_MA}" --dht-k "${K}" > "${SHARE_DIR}/callee.log" 2>&1 &
callee_pid=$!
for _ in $(seq 1 600); do
  [[ -s "${SHARE_DIR}/callee.ready" ]] && break
  sleep 0.1
done
[[ -s "${SHARE_DIR}/callee.ready" ]] || { tail -20 "${SHARE_DIR}/callee.log" >&2; echo "error: callee not ready" >&2; exit 1; }
callee_ma="$(sed -n 1p "${SHARE_DIR}/callee.ready")"
callee_account="$(sed -n 2p "${SHARE_DIR}/callee.ready")"
echo "callee: ${callee_ma} (record published)"

echo "stopping the seed: lookups must go through the Nodes"
compose stop seed
ctl find-all --timeout 180
echo "N-DHT PASSED"

echo "=== N-DHT-CALL: the caller knows the callee's PeerId only; the DHT gives the address ==="
NODE_MA="$(ctl seed-ma --host dht-1 --timeout 30)"
dead_ma="/ip4/192.0.2.1/udp/9/adp/1.0.0/p2p/${callee_ma##*/p2p/}"  # TEST-NET: never answers
docker exec pp-dht-lab-caller /probes/pp-call-probe --role offerer --product-stack \
  --peer "${dead_ma}" --peer-account "${callee_account}" --via-hop "${NODE_MA}" \
  --call-id "${call_id}" --signal-dir /share/sig --hold-ms 5000 --timeout-ms 60000 \
  --dht "${NODE_MA}" --dht-k "${K}" 2>&1 | tee "${SHARE_DIR}/caller.log"
rc=${PIPESTATUS[0]}
if [[ "${rc}" -ne 0 ]]; then
  echo "--- callee (tail) ---" >&2
  tail -30 "${SHARE_DIR}/callee.log" >&2
  echo "error: N-DHT-CALL failed (rc=${rc})" >&2
  exit 1
fi
grep -q "dht found" "${SHARE_DIR}/caller.log" || { echo "error: the caller did not use the DHT" >&2; exit 1; }
echo "N-DHT-CALL PASSED"
