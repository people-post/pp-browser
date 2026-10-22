#!/usr/bin/env bash
# DHT lab (N-DHT): an org seed and 12 DHT Nodes (k = 3) on an internal network. Once the routing
# tables fill, the seed stops; every Node must then find every other Node's record by lookup.
# Design: pp-cpp-mesh docs/DHT.md § 5. Compose: packaging/pp-node/docker-compose.dht-lab.yml.
#
# Usage: pp_dht_lab_smoke.sh [--keep]   (pp_local_test.sh --suite dht stages pp-node:local first)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
COMPOSE_FILE="${ROOT}/packaging/pp-node/docker-compose.dht-lab.yml"
NODES="${PP_DHT_LAB_NODES:-12}"
KEEP=0
[[ "${1:-}" == "--keep" ]] && KEEP=1

compose() { docker compose -f "${COMPOSE_FILE}" "$@"; }
ctl() { docker exec pp-dht-lab-ctl python3 /scripts/pp_dht_lab.py "$@" --nodes "${NODES}"; }

cleanup() {
  if [[ "${KEEP}" -eq 0 ]]; then
    compose down -v --remove-orphans >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

dht_services=()
for i in $(seq 1 "${NODES}"); do dht_services+=("dht-${i}"); done

echo "=== N-DHT: seed + ${NODES} DHT Nodes (k=${PP_DHT_LAB_K:-3}) ==="
compose down -v --remove-orphans >/dev/null 2>&1 || true
compose up -d --build seed ctl
SEED_MA="$(ctl seed-ma --timeout 60)"
echo "seed: ${SEED_MA}"

PP_DHT_SEED_MA="${SEED_MA}" compose up -d "${dht_services[@]}"
ctl wait-tables --min-contacts 3 --timeout 120

echo "stopping the seed: lookups must go through the Nodes"
compose stop seed
ctl find-all --timeout 180
echo "N-DHT PASSED"
