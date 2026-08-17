#!/usr/bin/env bash
# Local test driver: unit / call-direct / pp-node hop (Docker) lifecycle.
#
# Owns Docker up/stop/clear. Individual smokes stay in scripts/pp_*_smoke.sh.
# Default hop compose: packaging/pp-node/docker-compose.relay-smoke.yml
# (do not run alongside packaging/pp-node/docker-compose.yml — same host ports).
#
# Suites: unit | call | node | cap | soak | chaos | call-hop | all
# --suite node stays cheap (L0/L1/fanout + N-CAP N=4). Stress is cap/soak/chaos.
#
# See docs/ops/TEST_STRATEGY.md and packaging/pp-node/IMAGE_SMOKE.md
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPOSE_FILE="${PP_LOCAL_COMPOSE_FILE:-${ROOT}/packaging/pp-node/docker-compose.relay-smoke.yml}"
COMPOSE_PROJECT="${PP_LOCAL_COMPOSE_PROJECT:-pp-local-test}"
DOGFOOD_COMPOSE="${PP_LOCAL_DOGFOOD_COMPOSE:-${ROOT}/packaging/pp-node/docker-compose.yml}"
HOP_CONTAINER="${PP_LOCAL_HOP_CONTAINER:-pp-node-relay-smoke-hop}"
STATUS_URL="${PP_NODE_STATUS_URL:-http://127.0.0.1:18518}"
BUILD_DIR="${PP_LOCAL_BUILD_DIR:-${ROOT}/build}"
DOCKER_CONTEXT="${ROOT}/dist/pp-node/docker"
READY_FILE="${PP_CALL_PROBE_READY_FILE:-/tmp/pp-call-probe.ready}"
# gtest_discover_tests names are PascalCase fixture names (ctest -R is case-sensitive).
CTEST_REGEX='CallMediaDirect|MediaRelayService|CircuitCallMedia|CircuitMediaRelay|CircuitRelayService|CallLifecycle'

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
  clear     down -v relay-smoke + dogfood pp-node-local; remove ready-file
            --images also removes image pp-node:local
  status    compose ps + /healthz + /status
  build     cmake --build probes (pp-node-probe, pp-call-probe)

Options (run / up):
  --suite unit|call|node|cap|soak|chaos|call-hop|all
                               run only (default: all)
  --down                       after run, compose stop (not clear)
  --no-build                   skip compose --build on up
  --status-url URL             default \$PP_NODE_STATUS_URL or ${STATUS_URL}

Environment:
  PP_LOCAL_COMPOSE_FILE / PP_LOCAL_COMPOSE_PROJECT / PP_LOCAL_BUILD_DIR
  PP_NODE_STATUS_URL / PP_NODE_PROBE_ATTACHERS / PP_CALL_PROBE_CYCLES
  PP_NODE_CAP_SWEEP (default 4,8,12,16 for --suite cap)
  PP_NODE_PROBE_BRIDGES / PP_NODE_SOAK_SEC (default 120; weekly 3600)

Examples:
  $(basename "$0") run --suite unit
  $(basename "$0") run --suite node
  $(basename "$0") run --suite cap
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
  if [[ ! -f "${DOCKER_CONTEXT}/Dockerfile" ]]; then
    die "missing ${DOCKER_CONTEXT}/Dockerfile
Package the node image first (Ubuntu 24.04 family):
  PP_BROWSER_RELEASE_VERSION=0.0.0-local bash scripts/pp_node_package_linux.sh all"
  fi
}

cmake_build_probes() {
  need_cmd cmake
  if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    die "no CMake cache at ${BUILD_DIR}; configure a desktop tree first (docs/ops/BUILD.md)"
  fi
  echo "building probes + pp-node in ${BUILD_DIR}"
  cmake --build "${BUILD_DIR}" --target pp-node-probe pp-call-probe pp-node -j
}

# Hop image was packaged Aug 7; desktop probe is current. Staging a newer desktop
# pp-node avoids "ProtocolMuxer: protocol negotiation failed" on media-relay.
HOP_NEEDS_REBUILD=0
DESKTOP_NODE="${BUILD_DIR}/src/app/node/pp-node"
STAGED_NODE="${DOCKER_CONTEXT}/pp-node"

stage_hop_binary_if_newer() {
  if [[ ! -x "${DESKTOP_NODE}" ]]; then
    echo "warn: ${DESKTOP_NODE} missing; using staged hop binary as-is"
    return 0
  fi
  if [[ -f "${STAGED_NODE}" ]] && [[ ! "${DESKTOP_NODE}" -nt "${STAGED_NODE}" ]]; then
    return 0
  fi
  echo "staging newer desktop pp-node into ${STAGED_NODE} (hop image was stale vs probe)"
  mkdir -p "${DOCKER_CONTEXT}"
  if command -v strip >/dev/null 2>&1; then
    strip --strip-unneeded -o "${STAGED_NODE}" "${DESKTOP_NODE}"
  else
    cp -f "${DESKTOP_NODE}" "${STAGED_NODE}"
  fi
  chmod +x "${STAGED_NODE}"
  if [[ ! -f "${DOCKER_CONTEXT}/config.json.example" ]]; then
    cp -f "${ROOT}/packaging/pp-node/config.json.example" "${DOCKER_CONTEXT}/config.json.example"
  fi
  HOP_NEEDS_REBUILD=1
}

wait_healthz() {
  export PP_NODE_STATUS_URL="${STATUS_URL}"
  bash "${ROOT}/scripts/pp_node_image_smoke.sh" --status-url "${STATUS_URL}"
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
  if [[ -f "${DOGFOOD_COMPOSE}" ]]; then
    echo "compose down dogfood (${DOGFOOD_COMPOSE})"
    dogfood_compose down --remove-orphans >/dev/null 2>&1 || true
    docker stop pp-node-local >/dev/null 2>&1 || true
    docker rm pp-node-local >/dev/null 2>&1 || true
  fi
  rm -f "${READY_FILE}"
  if [[ "${remove_images}" -eq 1 ]]; then
    docker image rm pp-node:local 2>/dev/null || true
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
  bash "${ROOT}/scripts/pp_call_direct_smoke.sh"
}

run_node() {
  cmake_build_probes
  cmd_up
  echo "=== suite node (L0/L1/N-FANOUT/N-CAP N=4) ==="
  export PP_NODE_STATUS_URL="${STATUS_URL}"
  bash "${ROOT}/scripts/pp_node_relay_smoke.sh" --status-url "${STATUS_URL}"
  bash "${ROOT}/scripts/pp_node_fanout_smoke.sh" --status-url "${STATUS_URL}"
  bash "${ROOT}/scripts/pp_node_cap_smoke.sh" --status-url "${STATUS_URL}"
}

run_cap() {
  cmake_build_probes
  cmd_up
  echo "=== suite cap (N-CAP-MEDIA sweep + N-CAP-CIRCUIT) ==="
  export PP_NODE_STATUS_URL="${STATUS_URL}"
  export PP_NODE_CAP_SWEEP="${PP_NODE_CAP_SWEEP:-4,8,12,16}"
  export PP_NODE_PROBE_BRIDGES="${PP_NODE_PROBE_BRIDGES:-4,8}"
  bash "${ROOT}/scripts/pp_node_cap_smoke.sh" --status-url "${STATUS_URL}" --sweep "${PP_NODE_CAP_SWEEP}"
  bash "${ROOT}/scripts/pp_node_circuit_cap_smoke.sh" --status-url "${STATUS_URL}" --bridges "${PP_NODE_PROBE_BRIDGES}"
}

run_soak() {
  cmake_build_probes
  cmd_up
  echo "=== suite soak (N-SOAK ${PP_NODE_SOAK_SEC:-120}s) ==="
  export PP_NODE_STATUS_URL="${STATUS_URL}"
  bash "${ROOT}/scripts/pp_node_soak_smoke.sh" --status-url "${STATUS_URL}"
}

run_chaos() {
  cmake_build_probes
  cmd_up
  echo "=== suite chaos (N-CHAOS) ==="
  export PP_NODE_STATUS_URL="${STATUS_URL}"
  bash "${ROOT}/scripts/pp_node_chaos_smoke.sh" --status-url "${STATUS_URL}"
}

run_call_hop() {
  cmake_build_probes
  cmd_up
  echo "=== suite call-hop (B-CALL-HOP) ==="
  export PP_NODE_STATUS_URL="${STATUS_URL}"
  bash "${ROOT}/scripts/pp_call_hop_smoke.sh" --status-url "${STATUS_URL}"
}

cmd_run() {
  case "${SUITE}" in
    unit) run_unit ;;
    call) run_call ;;
    node) run_node ;;
    cap) run_cap ;;
    soak) run_soak ;;
    chaos) run_chaos ;;
    call-hop) run_call_hop ;;
    all)
      run_unit
      run_call
      run_node
      ;;
    *) die "unknown --suite ${SUITE} (unit|call|node|cap|soak|chaos|call-hop|all)" ;;
  esac
  if [[ "${DOWN_AFTER}" -eq 1 ]]; then
    cmd_stop
  elif [[ "${SUITE}" =~ ^(node|cap|soak|chaos|call-hop|all)$ ]]; then
    echo "hop left running; $(basename "$0") stop | clear when done"
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
