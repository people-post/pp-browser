#!/usr/bin/env bash
# Local test driver: unit / call-direct / pp-node hop (Docker) lifecycle.
#
# Owns Docker up/stop/clear. Individual smokes stay in scripts/test/pp_*_smoke.sh.
# Default hop compose: packaging/pp-node/docker-compose.relay-smoke.yml
# (do not run alongside packaging/pp-node/docker-compose.yml — same host ports).
#
# Suites: unit | call | conflict | msg-call | node | cap | soak | chaos | call-hop | msg-call-hop | mix | hard | hard-w2 | hard-w3 | hard-w5 | dht | app | app-nat | all
# --suite node stays cheap (L0/L1/fanout + N-CAP N=4). Stress is cap/soak/chaos.
# --suite mix is nightly: browser parallel + shrunk hop parallel + same-session hop chat.
# --suite hard is Wave 1 forced-hop (isolated nets; separate compose/ports 18618).
# --suite hard-w2 is Wave 2 netem/tbf (N-HARD-LOSSY / BW) on the same compose.
# --suite hard-w3 is Wave 3 discovery (N-HARD-STALE-ADDR / SEED-ONLY).
# --suite dht is N-DHT: a seed + 12 DHT Nodes (k = 3); the seed stops, every Node finds every other.
# --suite app is the app lab: scripted users on the real app stack (pp-app-probe) over the real
#   relay (web2 www + messenger, PP_WEB2_ROOT) — glare, redial storms, restarts, a peer vanishing.
# --suite app-nat runs the app lab with each user in its own container behind its own NAT gateway
#   (docker-compose.app-lab-nat.yml): no two users can reach each other directly.
#
# See docs/ops/TEST_STRATEGY.md, packaging/pp-node/IMAGE_SMOKE.md, packaging/pp-node/HARD_LAB.md
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
COMPOSE_FILE="${PP_LOCAL_COMPOSE_FILE:-${ROOT}/packaging/pp-node/docker-compose.relay-smoke.yml}"
COMPOSE_PROJECT="${PP_LOCAL_COMPOSE_PROJECT:-pp-local-test}"
DOGFOOD_COMPOSE="${PP_LOCAL_DOGFOOD_COMPOSE:-${ROOT}/packaging/pp-node/docker-compose.yml}"
HARD_COMPOSE_FILE="${PP_HARD_COMPOSE_FILE:-${ROOT}/packaging/pp-node/docker-compose.hard-lab.yml}"
HARD_COMPOSE_PROJECT="${PP_HARD_COMPOSE_PROJECT:-pp-hard-lab}"
HARD_STATUS_URL="${PP_HARD_STATUS_URL:-http://127.0.0.1:18618}"
HARD_HOP_CONTAINER="${PP_HARD_HOP_CONTAINER:-pp-hard-lab-hop}"
HOP_CONTAINER="${PP_LOCAL_HOP_CONTAINER:-pp-node-relay-smoke-hop}"
STATUS_URL="${PP_NODE_STATUS_URL:-http://127.0.0.1:18518}"
# Absolute path required: compose treats unprefixed relative mounts as named volumes
# (e.g. PP_LOCAL_BUILD_DIR=build → "undefined volume build/src/app/node").
BUILD_DIR="$(cd "${PP_LOCAL_BUILD_DIR:-${ROOT}/build}" && pwd)"
DOCKER_CONTEXT="${ROOT}/dist/pp-node/docker"
READY_FILE="${PP_CALL_PROBE_READY_FILE:-/tmp/pp-call-probe.ready}"
# gtest_discover_tests names are PascalCase fixture names (ctest -R is case-sensitive).
CTEST_REGEX='CallMediaDirect|MediaRelayService|CircuitCallMedia|CircuitMediaRelay|CircuitRelayService|CallLifecycle|CallTxOnly|CallListenAddrs|CallAnswererKick|CallMediaBridgeAnswerer|InboundSfuAttachIgnoredWhenStatus|AmpDirectChat'

SUITE="all"
DOWN_AFTER=0
COMPOSE_BUILD=1

usage() {
  cat <<EOF
Usage: $(basename "$0") <command> [options]

Commands:
  run       Build probes as needed; run --suite (default: all). Leaves hop up
            unless --down.
  up        Stop other hops on 18517/18518, start relay-smoke hop, wait /healthz
  stop      Stop relay-smoke hop (volume kept)
  clear     down -v relay-smoke + dogfood pp-node-local + hard-lab; remove ready-file
            --images also removes image pp-node:local / pp-hard-peer:local
  status    compose ps + /healthz + /status (relay-smoke; hard if up)
  build     cmake --build probes (pp-node-probe, pp-call-probe)

Options (run / up):
  --suite unit|call|node|cap|soak|chaos|call-hop|msg-call-hop|conflict|msg-call|mix|hard|hard-w2|hard-w3|hard-w5|dht|ledger|app|app-nat|all
                               run only (default: all)
  --down                       after run, compose stop (not clear)
  --no-build                   skip compose --build on up
  --status-url URL             default \$PP_NODE_STATUS_URL or ${STATUS_URL}

Environment:
  PP_LOCAL_COMPOSE_FILE / PP_LOCAL_COMPOSE_PROJECT / PP_LOCAL_BUILD_DIR
  PP_NODE_STATUS_URL / PP_NODE_PROBE_ATTACHERS / PP_CALL_PROBE_CYCLES
  PP_NODE_CAP_SWEEP (default 4,8,12,16 for --suite cap)
  PP_NODE_PROBE_BRIDGES / PP_NODE_SOAK_SEC (default 120; weekly 3600)
  PP_MIX_BRIDGES (default 2) / PP_MIX_CALL_HOP_CYCLES (default 2)
  PP_HARD_COMPOSE_FILE / PP_HARD_STATUS_URL (default http://127.0.0.1:18618)
  PP_LEDGER_ROOT / PP_LEDGER_BUILD_DIR (--suite ledger: a pp-ledger checkout with its apps built)
  PP_WEB2_ROOT (--suite app: a web2 checkout; default ../web2 beside web3) / PP_APP_LAB_ARGS
  PP_APP_LAB_WWW_IMAGE + PP_APP_LAB_MESSENGER_IMAGE (--suite app: the relay from these images instead)

Examples:
  $(basename "$0") run --suite unit
  $(basename "$0") run --suite call
  $(basename "$0") run --suite conflict
  $(basename "$0") run --suite msg-call
  $(basename "$0") run --suite node
  $(basename "$0") run --suite cap
  $(basename "$0") run --suite mix
  $(basename "$0") run --suite msg-call-hop
  $(basename "$0") run --suite hard
  $(basename "$0") run --suite hard-w2
  $(basename "$0") run --suite hard-w3
  $(basename "$0") run --suite hard-w5
  $(basename "$0") run --suite dht
  $(basename "$0") up && $(basename "$0") status
  $(basename "$0") stop
  $(basename "$0") clear --images
EOF
}

die() {
  echo "error: $*" >&2
  exit 1
}

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "missing command: $1"
}

compose() {
  need_cmd docker
  docker compose -p "${COMPOSE_PROJECT}" -f "${COMPOSE_FILE}" "$@"
}

hard_compose() {
  need_cmd docker
  mkdir -p /tmp/pp-hard-lab-share
  PP_HARD_PROBE_DIR="${BUILD_DIR}/src/app/node" \
    PP_HARD_SHARE_DIR="${PP_HARD_SHARE_DIR:-/tmp/pp-hard-lab-share}" \
    docker compose -p "${HARD_COMPOSE_PROJECT}" -f "${HARD_COMPOSE_FILE}" "$@"
}

dogfood_compose() {
  need_cmd docker
  docker compose -f "${DOGFOOD_COMPOSE}" "$@"
}

hop_healthy() {
  curl -fsS -m 2 "${STATUS_URL}/healthz" >/dev/null 2>&1
}

hop_ports_published() {
  docker port "${HOP_CONTAINER}" 18518 >/dev/null 2>&1
}

# Stop anything else publishing the hop/status ports (usually container pp-node-local).
free_hop_ports() {
  need_cmd docker
  if [[ -f "${DOGFOOD_COMPOSE}" ]]; then
    echo "stopping dogfood compose (${DOGFOOD_COMPOSE}) if present"
    dogfood_compose stop >/dev/null 2>&1 || true
    docker stop pp-node-local >/dev/null 2>&1 || true
  fi
  local id name
  while read -r id; do
    [[ -z "${id}" ]] && continue
    name="$(docker inspect -f '{{.Name}}' "${id}" 2>/dev/null | sed 's#^/##')"
    if [[ "${name}" == "${HOP_CONTAINER}" ]]; then
      continue
    fi
    echo "stopping container ${name} (${id}) holding 18517/18518"
    docker stop "${id}" >/dev/null || true
  done < <({ docker ps -q --filter publish=18517; docker ps -q --filter publish=18518; } | sort -u)
}

ensure_docker_context() {
  if [[ -f "${DOCKER_CONTEXT}/Dockerfile" ]]; then
    return 0
  fi
  # A fresh checkout has no packaged context: stage it from the desktop build
  # (pp-node + Dockerfile + config is all the image needs).
  if [[ -x "${DESKTOP_NODE}" ]]; then
    echo "no packaged hop context; staging it from ${DESKTOP_NODE}"
    stage_hop_binary_if_newer
    return 0
  fi
  die "missing ${DOCKER_CONTEXT}/Dockerfile and ${DESKTOP_NODE}
Build pp-node in ${BUILD_DIR}, or package the node image (Ubuntu 24.04 family):
  bash scripts/platform/pp_node_package_linux.sh all"
}

cmake_build_probes() {
  need_cmd cmake
  if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    die "no CMake cache at ${BUILD_DIR}; configure a desktop tree first (docs/ops/BUILD.md)"
  fi
  echo "building probes + pp-node in ${BUILD_DIR}"
  cmake --build "${BUILD_DIR}" --target pp-node-probe pp-call-probe pp-node -j
}

# Keep staged hop binary + packaging files in sync with the desktop build / Amp
# packaging tree (stale TCP listen_multiaddr configs break Amp-only hops).
HOP_NEEDS_REBUILD=0
DESKTOP_NODE="${BUILD_DIR}/src/app/node/pp-node"
STAGED_NODE="${DOCKER_CONTEXT}/pp-node"

stage_hop_packaging() {
  mkdir -p "${DOCKER_CONTEXT}"
  local pkg_cfg="${ROOT}/packaging/pp-node/config.json.example"
  local pkg_df="${ROOT}/packaging/pp-node/Dockerfile"
  local staged_cfg="${DOCKER_CONTEXT}/config.json.example"
  local staged_df="${DOCKER_CONTEXT}/Dockerfile"
  if [[ ! -f "${staged_cfg}" ]] || ! cmp -s "${pkg_cfg}" "${staged_cfg}"; then
    cp -f "${pkg_cfg}" "${staged_cfg}"
    HOP_NEEDS_REBUILD=1
  fi
  if [[ ! -f "${staged_df}" ]] || ! cmp -s "${pkg_df}" "${staged_df}"; then
    cp -f "${pkg_df}" "${staged_df}"
    HOP_NEEDS_REBUILD=1
  fi
}

stage_hop_binary_if_newer() {
  stage_hop_packaging
  if [[ ! -x "${DESKTOP_NODE}" ]]; then
    echo "warn: ${DESKTOP_NODE} missing; using staged hop binary as-is"
    return 0
  fi
  if [[ -f "${STAGED_NODE}" ]] && [[ ! "${DESKTOP_NODE}" -nt "${STAGED_NODE}" ]]; then
    return 0
  fi
  echo "staging newer desktop pp-node into ${STAGED_NODE} (hop image was stale vs probe)"
  if command -v strip >/dev/null 2>&1; then
    strip --strip-unneeded -o "${STAGED_NODE}" "${DESKTOP_NODE}"
  else
    cp -f "${DESKTOP_NODE}" "${STAGED_NODE}"
  fi
  chmod +x "${STAGED_NODE}"
  HOP_NEEDS_REBUILD=1
}

wait_healthz() {
  export PP_NODE_STATUS_URL="${STATUS_URL}"
  bash "${ROOT}/scripts/test/pp_node_image_smoke.sh" --status-url "${STATUS_URL}"
}

cmd_up() {
  ensure_docker_context
  stage_hop_binary_if_newer
  if hop_healthy && hop_ports_published && [[ "${HOP_NEEDS_REBUILD}" -eq 0 ]]; then
    echo "reusing healthy hop ${HOP_CONTAINER} at ${STATUS_URL}"
    wait_healthz
    return 0
  fi
  free_hop_ports
  # A previous bind failure can leave the hop running with Networks={} (no published
  # ports). Recreate so compose attaches the default network and host port maps.
  local args=(up -d --force-recreate)
  if [[ "${COMPOSE_BUILD}" -eq 1 || "${HOP_NEEDS_REBUILD}" -eq 1 ]]; then
    args+=(--build)
  fi
  echo "compose up --force-recreate project=${COMPOSE_PROJECT} file=${COMPOSE_FILE}"
  compose "${args[@]}"
  if ! hop_ports_published; then
    die "hop ${HOP_CONTAINER} has no published 18518 (Docker network missing).
Try: $(basename "$0") clear && $(basename "$0") up"
  fi
  wait_healthz
}

cmd_stop() {
  echo "compose stop project=${COMPOSE_PROJECT}"
  compose stop
}

cmd_clear() {
  local remove_images=0
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --images) remove_images=1; shift ;;
      *) die "unknown clear option: $1" ;;
    esac
  done
  echo "compose down -v project=${COMPOSE_PROJECT}"
  compose down -v --remove-orphans || true
  echo "compose down -v hard-lab project=${HARD_COMPOSE_PROJECT}"
  hard_compose down -v --remove-orphans || true
  if [[ -f "${DOGFOOD_COMPOSE}" ]]; then
    echo "compose down dogfood (${DOGFOOD_COMPOSE})"
    dogfood_compose down --remove-orphans >/dev/null 2>&1 || true
    docker stop pp-node-local >/dev/null 2>&1 || true
    docker rm pp-node-local >/dev/null 2>&1 || true
  fi
  rm -f "${READY_FILE}"
  rm -rf /tmp/pp-hard-lab-share
  if [[ "${remove_images}" -eq 1 ]]; then
    docker image rm pp-node:local 2>/dev/null || true
    docker image rm pp-hard-peer:local 2>/dev/null || true
  fi
  echo "pp-local-test clear done"
}

cmd_status() {
  echo "=== compose ps (${COMPOSE_PROJECT}) ==="
  compose ps || true
  echo "=== ${STATUS_URL}/healthz ==="
  curl -fsS -m 5 "${STATUS_URL}/healthz" || echo "(unreachable)"
  echo
  echo "=== ${STATUS_URL}/status ==="
  curl -fsS -m 5 "${STATUS_URL}/status" || echo "(unreachable)"
  echo
  echo "=== hard-lab compose ps (${HARD_COMPOSE_PROJECT}) ==="
  hard_compose ps || true
  echo "=== ${HARD_STATUS_URL}/healthz ==="
  curl -fsS -m 5 "${HARD_STATUS_URL}/healthz" || echo "(unreachable)"
  echo
}

cmd_build() {
  cmake_build_probes
}

run_unit() {
  need_cmd ctest
  if [[ ! -d "${BUILD_DIR}" ]]; then
    die "build dir missing: ${BUILD_DIR}"
  fi
  echo "=== suite unit (core compose gtests) ==="
  ctest --test-dir "${BUILD_DIR}" -R "${CTEST_REGEX}" --output-on-failure --no-tests=error
}

run_call() {
  cmake_build_probes
  echo "=== suite call (B-CALL-DIRECT) ==="
  bash "${ROOT}/scripts/test/pp_call_direct_smoke.sh"
}

APP_LAB_COMPOSE="${ROOT}/packaging/pp-node/docker-compose.app-lab.yml"
APP_LAB_NAT_COMPOSE="${ROOT}/packaging/pp-node/docker-compose.app-lab-nat.yml"
# The relay from published images (both set) instead of PP_WEB2_ROOT: the nightly lab.
APP_LAB_IMAGES_COMPOSE="${ROOT}/packaging/pp-node/docker-compose.app-lab-images.yml"
APP_LAB_NAT=0  # --suite app-nat: users behind their own NATs

app_lab_compose() {
  need_cmd docker
  local files=(-f "${APP_LAB_COMPOSE}")
  if [[ "${APP_LAB_NAT}" -eq 1 ]]; then
    files+=(-f "${APP_LAB_NAT_COMPOSE}")
  fi
  if [[ -n "${PP_APP_LAB_WWW_IMAGE:-}" || -n "${PP_APP_LAB_MESSENGER_IMAGE:-}" ]]; then
    [[ -n "${PP_APP_LAB_WWW_IMAGE:-}" && -n "${PP_APP_LAB_MESSENGER_IMAGE:-}" ]] ||
      die "relay images: set both PP_APP_LAB_WWW_IMAGE and PP_APP_LAB_MESSENGER_IMAGE"
    files+=(-f "${APP_LAB_IMAGES_COMPOSE}")
  fi
  PP_WEB2_ROOT="${PP_WEB2_ROOT:-$(cd "${ROOT}/../.." && pwd)/web2}" \
    PP_APP_LAB_PROBE_DIR="${BUILD_DIR}/src/app/node" PP_APP_LAB_NAT_SHARE="${PP_APP_LAB_NAT_SHARE:-/tmp/pp-app-lab-nat}" \
    docker compose -p pp-app-lab "${files[@]}" "$@"
}

# The lab seed's address in the lab directory: it carries the seed's PeerId (#385).
lab_hop_advertise() {
  local peer
  peer="$(curl -fsS -m 2 http://127.0.0.1:18718/status 2>/dev/null |
    python3 -c 'import json,sys; print(json.load(sys.stdin).get("peer_id",""))' 2>/dev/null || true)"
  if [[ -n "${peer}" ]]; then
    export PP_APP_LAB_HOP_ADVERTISE="/ip4/198.51.100.10/udp/18517/adp/1.0.0/p2p/${peer}"
    export PP_APP_LAB_HOP_PUBLISH=true
  fi
}

run_app() {
  need_cmd cmake
  need_cmd python3
  ensure_docker_context
  lab_hop_advertise  # a seed already running keeps its address (no recreate)
  stage_hop_binary_if_newer
  cmake --build "${BUILD_DIR}" --target pp-app-probe -j
  local args=(up -d)
  if [[ "${COMPOSE_BUILD}" -eq 1 || "${HOP_NEEDS_REBUILD}" -eq 1 ]]; then
    args+=(--build)
  fi
  if [[ "${APP_LAB_NAT}" -eq 1 ]]; then
    mkdir -p "${PP_APP_LAB_NAT_SHARE:-/tmp/pp-app-lab-nat}"
    app_lab_compose build gw-a user-a  # the gateway and user images (cached when unchanged)
  fi
  app_lab_compose "${args[@]}"
  local i
  for i in $(seq 1 60); do
    curl -fsS -m 2 http://127.0.0.1:18719/healthz >/dev/null 2>&1 &&
      curl -fsS -m 2 http://127.0.0.1:18718/healthz >/dev/null 2>&1 && break
    sleep 1
  done
  if [[ -z "${PP_APP_LAB_HOP_ADVERTISE:-}" ]]; then
    # A new seed has just minted its PeerId: start it again listed in the lab directory, as prod's are.
    lab_hop_advertise
    app_lab_compose up -d hop
    for i in $(seq 1 60); do
      curl -fsS -m 2 http://127.0.0.1:18718/healthz >/dev/null 2>&1 && break
      sleep 1
    done
  fi
  local nat_args=()
  if [[ "${APP_LAB_NAT}" -eq 1 ]]; then
    echo "=== suite app-nat (scripted users, each behind its own NAT, over the lab relay and hop) ==="
    nat_args=(--nat)
  else
    echo "=== suite app (scripted users on pp-app-probe over the lab relay) ==="
  fi
  # shellcheck disable=SC2086 # PP_APP_LAB_ARGS is a word list (e.g. "--only redial_storm --repeat 3")
  python3 "${ROOT}/scripts/test/pp_app_lab.py" --probe "${BUILD_DIR}/src/app/node/pp-app-probe" \
    ${nat_args[@]+"${nat_args[@]}"} ${PP_APP_LAB_ARGS:-}
}

run_ledger() {
  need_cmd cmake
  cmake --build "${BUILD_DIR}" --target pp-node -j
  echo "=== suite ledger (N-LEDGER: pp-node as pp-ledger relay, then miner; local processes) ==="
  PP_LOCAL_BUILD_DIR="${BUILD_DIR}" bash "${ROOT}/scripts/test/pp_node_ledger_smoke.sh" all
}

run_conflict() {
  cmake_build_probes
  echo "=== suite conflict (B-CONFLICT) ==="
  bash "${ROOT}/scripts/test/pp_call_conflict_smoke.sh"
}

run_msg_call() {
  cmake_build_probes
  echo "=== suite msg-call (B-MSG+CALL) ==="
  bash "${ROOT}/scripts/test/pp_call_msg_smoke.sh"
}

run_node() {
  cmake_build_probes
  cmd_up
  echo "=== suite node (L0/L1/N-FANOUT/N-CAP N=4) ==="
  export PP_NODE_STATUS_URL="${STATUS_URL}"
  bash "${ROOT}/scripts/test/pp_node_relay_smoke.sh" --status-url "${STATUS_URL}"
  bash "${ROOT}/scripts/test/pp_node_fanout_smoke.sh" --status-url "${STATUS_URL}"
  bash "${ROOT}/scripts/test/pp_node_cap_smoke.sh" --status-url "${STATUS_URL}"
}

run_cap() {
  cmake_build_probes
  cmd_up
  echo "=== suite cap (N-CAP-MEDIA sweep + N-CAP-CIRCUIT) ==="
  export PP_NODE_STATUS_URL="${STATUS_URL}"
  export PP_NODE_CAP_SWEEP="${PP_NODE_CAP_SWEEP:-4,8,12,16}"
  export PP_NODE_PROBE_BRIDGES="${PP_NODE_PROBE_BRIDGES:-4,8}"
  bash "${ROOT}/scripts/test/pp_node_cap_smoke.sh" --status-url "${STATUS_URL}" --sweep "${PP_NODE_CAP_SWEEP}"
  bash "${ROOT}/scripts/test/pp_node_circuit_cap_smoke.sh" --status-url "${STATUS_URL}" --bridges "${PP_NODE_PROBE_BRIDGES}"
}

run_soak() {
  cmake_build_probes
  cmd_up
  echo "=== suite soak (N-SOAK ${PP_NODE_SOAK_SEC:-120}s) ==="
  export PP_NODE_STATUS_URL="${STATUS_URL}"
  bash "${ROOT}/scripts/test/pp_node_soak_smoke.sh" --status-url "${STATUS_URL}"
}

run_chaos() {
  cmake_build_probes
  cmd_up
  echo "=== suite chaos (N-CHAOS) ==="
  export PP_NODE_STATUS_URL="${STATUS_URL}"
  bash "${ROOT}/scripts/test/pp_node_chaos_smoke.sh" --status-url "${STATUS_URL}"
}

run_call_hop() {
  cmake_build_probes
  cmd_up
  echo "=== suite call-hop (B-CALL-HOP) ==="
  export PP_NODE_STATUS_URL="${STATUS_URL}"
  bash "${ROOT}/scripts/test/pp_call_hop_smoke.sh" --status-url "${STATUS_URL}"
}

run_msg_call_hop() {
  cmake_build_probes
  cmd_up
  echo "=== suite msg-call-hop (B-MSG+CALL via hop) ==="
  export PP_NODE_STATUS_URL="${STATUS_URL}"
  bash "${ROOT}/scripts/test/pp_call_hop_msg_smoke.sh" --status-url "${STATUS_URL}"
}

run_mix() {
  cmake_build_probes
  echo "=== suite mix (B-MIX browser interference) ==="
  bash "${ROOT}/scripts/test/pp_mix_browser_smoke.sh"
  cmd_up
  echo "=== suite mix (N-MIX hop interference) ==="
  export PP_NODE_STATUS_URL="${STATUS_URL}"
  bash "${ROOT}/scripts/test/pp_mix_hop_smoke.sh" --status-url "${STATUS_URL}"
  echo "=== suite mix (same-session hop chat) ==="
  bash "${ROOT}/scripts/test/pp_call_hop_msg_smoke.sh" --status-url "${STATUS_URL}"
}

run_hard() {
  cmake_build_probes
  stage_hop_binary_if_newer
  ensure_docker_context
  # B-HARD-CALL retired: B-HARD-MSG+CALL is its superset (same call, plus chat).
  echo "=== suite hard (Wave 1: N-HARD-FORCE + B-HARD-MSG+CALL) ==="
  export PP_HARD_STATUS_URL="${HARD_STATUS_URL}"
  export PP_HARD_PROBE_DIR="${BUILD_DIR}/src/app/node"
  export PP_HARD_SHARE_DIR="${PP_HARD_SHARE_DIR:-/tmp/pp-hard-lab-share}"
  local args=(up -d --force-recreate)
  if [[ "${COMPOSE_BUILD}" -eq 1 || "${HOP_NEEDS_REBUILD}" -eq 1 ]]; then
    args+=(--build)
  fi
  hard_compose "${args[@]}"
  bash "${ROOT}/scripts/test/pp_hard_force_smoke.sh" --status-url "${HARD_STATUS_URL}" --skip-up
  bash "${ROOT}/scripts/test/pp_hard_call_smoke.sh" --status-url "${HARD_STATUS_URL}" --skip-up --with-chat
}

run_hard_w2() {
  cmake_build_probes
  stage_hop_binary_if_newer
  ensure_docker_context
  # N-HARD-ASYM retired: it ran the same force smoke as LOSSY with no asymmetric-specific gate.
  echo "=== suite hard-w2 (Wave 2: N-HARD-LOSSY + N-HARD-BW) ==="
  export PP_HARD_STATUS_URL="${HARD_STATUS_URL}"
  export PP_HARD_PROBE_DIR="${BUILD_DIR}/src/app/node"
  export PP_HARD_SHARE_DIR="${PP_HARD_SHARE_DIR:-/tmp/pp-hard-lab-share}"
  local args=(up -d --force-recreate)
  if [[ "${COMPOSE_BUILD}" -eq 1 || "${HOP_NEEDS_REBUILD}" -eq 1 ]]; then
    args+=(--build)
  fi
  hard_compose "${args[@]}"
  bash "${ROOT}/scripts/test/pp_hard_link_smoke.sh" --status-url "${HARD_STATUS_URL}" --skip-up --profile lossy
  bash "${ROOT}/scripts/test/pp_hard_link_smoke.sh" --status-url "${HARD_STATUS_URL}" --skip-up --profile bw
}

run_hard_w3() {
  cmake_build_probes
  stage_hop_binary_if_newer
  ensure_docker_context
  echo "=== suite hard-w3 (Wave 3: N-HARD-STALE-ADDR + N-HARD-SEED-ONLY) ==="
  export PP_HARD_STATUS_URL="${HARD_STATUS_URL}"
  export PP_HARD_PROBE_DIR="${BUILD_DIR}/src/app/node"
  export PP_HARD_SHARE_DIR="${PP_HARD_SHARE_DIR:-/tmp/pp-hard-lab-share}"
  local args=(up -d --force-recreate)
  if [[ "${COMPOSE_BUILD}" -eq 1 || "${HOP_NEEDS_REBUILD}" -eq 1 ]]; then
    args+=(--build)
  fi
  hard_compose "${args[@]}"
  bash "${ROOT}/scripts/test/pp_hard_disco_smoke.sh" --status-url "${HARD_STATUS_URL}" --skip-up --profile stale-addr
  bash "${ROOT}/scripts/test/pp_hard_disco_smoke.sh" --status-url "${HARD_STATUS_URL}" --skip-up --profile seed-only
}

run_hard_w5() {
  cmake_build_probes
  stage_hop_binary_if_newer
  ensure_docker_context
  echo "=== suite hard-w5 (Wave 5: N-HARD-CGNAT-ISH + STACK + COLD reach + UPGRADE/FLIP + PRIVATE + GROUP-MOVE + BCAST) ==="
  export PP_HARD_CGNAT_STATUS_URL="${PP_HARD_CGNAT_STATUS_URL:-http://127.0.0.1:18628}"
  export PP_HARD_PROBE_DIR="${BUILD_DIR}/src/app/node"
  export PP_HARD_CGNAT_SHARE_DIR="${PP_HARD_CGNAT_SHARE_DIR:-/tmp/pp-hard-lab-cgnat-share}"
  export PP_HARD_NAT_CALL_EXPECT="${PP_HARD_NAT_CALL_EXPECT:-success}"
  export PP_HARD_NAT_PHASE="${PP_HARD_NAT_PHASE:-all}"
  # Smoke owns CGNAT compose up/build (separate project from Wave 1 hard-lab).
  bash "${ROOT}/scripts/test/pp_hard_nat_smoke.sh" --status-url "${PP_HARD_CGNAT_STATUS_URL}"
}

run_dht() {
  cmake_build_probes
  stage_hop_binary_if_newer
  ensure_docker_context
  echo "=== suite dht (N-DHT: lookups with the seed down) ==="
  # The lab's compose builds pp-node:local from the staged hop binary (seed service).
  bash "${ROOT}/scripts/test/pp_dht_lab_smoke.sh"
}

cmd_run() {
  case "${SUITE}" in
    unit) run_unit ;;
    call) run_call ;;
    conflict) run_conflict ;;
    msg-call) run_msg_call ;;
    node) run_node ;;
    cap) run_cap ;;
    soak) run_soak ;;
    chaos) run_chaos ;;
    call-hop) run_call_hop ;;
    msg-call-hop) run_msg_call_hop ;;
    mix) run_mix ;;
    hard) run_hard ;;
    hard-w2) run_hard_w2 ;;
    hard-w3) run_hard_w3 ;;
    hard-w5) run_hard_w5 ;;
    dht) run_dht ;;
    ledger) run_ledger ;;
    app) run_app ;;
    app-nat)
      APP_LAB_NAT=1
      run_app
      ;;
    all)
      run_unit
      run_call
      run_conflict
      run_msg_call
      run_node
      ;;
    *) die "unknown --suite ${SUITE} (unit|call|conflict|msg-call|node|cap|soak|chaos|call-hop|msg-call-hop|mix|hard|hard-w2|hard-w3|hard-w5|dht|ledger|app|app-nat|all)" ;;
  esac
  if [[ "${DOWN_AFTER}" -eq 1 ]]; then
    if [[ "${SUITE}" == "app" || "${SUITE}" == "app-nat" ]]; then
      echo "app-lab compose stop project=pp-app-lab"
      app_lab_compose stop || true
    elif [[ "${SUITE}" == "hard-w5" ]]; then
      echo "hard-lab-cgnat compose stop project=pp-hard-lab-cgnat"
      docker compose -p pp-hard-lab-cgnat -f "${ROOT}/packaging/pp-node/docker-compose.hard-lab-cgnat.yml" stop || true
    elif [[ "${SUITE}" == "hard" || "${SUITE}" == "hard-w2" || "${SUITE}" == "hard-w3" ]]; then
      echo "hard-lab compose stop project=${HARD_COMPOSE_PROJECT}"
      hard_compose stop
    else
      cmd_stop
    fi
  elif [[ "${SUITE}" =~ ^(node|cap|soak|chaos|call-hop|msg-call-hop|mix|all)$ ]]; then
    echo "hop left running; $(basename "$0") stop | clear when done"
  elif [[ "${SUITE}" == "app" ]]; then
    echo "app-lab left running; stop with: docker compose -p pp-app-lab -f ${APP_LAB_COMPOSE} stop"
  elif [[ "${SUITE}" == "app-nat" ]]; then
    echo "app-lab left running; stop with: docker compose -p pp-app-lab -f ${APP_LAB_COMPOSE} -f ${APP_LAB_NAT_COMPOSE} stop"
  elif [[ "${SUITE}" == "hard-w5" ]]; then
    echo "hard-lab-cgnat left running; $(basename "$0") clear when done"
  elif [[ "${SUITE}" == "hard" || "${SUITE}" == "hard-w2" || "${SUITE}" == "hard-w3" ]]; then
    echo "hard-lab left running; $(basename "$0") clear when done (or: docker compose -p ${HARD_COMPOSE_PROJECT} -f ${HARD_COMPOSE_FILE} stop)"
  fi
  echo "pp-local-test run PASSED suite=${SUITE}"
}

if [[ $# -lt 1 ]]; then
  usage
  exit 2
fi

CMD="$1"
shift

while [[ $# -gt 0 ]]; do
  case "$1" in
    --suite) SUITE="$2"; shift 2 ;;
    --down) DOWN_AFTER=1; shift ;;
    --no-build) COMPOSE_BUILD=0; shift ;;
    --status-url) STATUS_URL="$2"; shift 2 ;;
    --images)
      # forwarded to clear
      break
      ;;
    -h|--help) usage; exit 0 ;;
    *)
      if [[ "${CMD}" == "clear" ]]; then
        break
      fi
      die "unknown option: $1"
      ;;
  esac
done

case "${CMD}" in
  run) cmd_run ;;
  up) cmd_up ;;
  stop) cmd_stop ;;
  clear) cmd_clear "$@" ;;
  status) cmd_status ;;
  build) cmd_build ;;
  -h|--help) usage ;;
  *)
    echo "error: unknown command: ${CMD}" >&2
    usage
    exit 2
    ;;
esac
