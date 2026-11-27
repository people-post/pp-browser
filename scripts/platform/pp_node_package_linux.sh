#!/usr/bin/env bash
# Build a stripped Linux pp-node binary, tarball, and Docker build context — or pp-node-plus's
# (PP_NODE_EDITION=plus: pp-node with the add-on services, projects/node-relay NR001; same tree, same version).
# Intended for Ubuntu 24.04 (same family as packaging/pp-node/Dockerfile and
# release CI runners). See docs/ops/RELEASE.md.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
case "${PP_NODE_EDITION:-node}" in
  node) NAME=pp-node ;;
  plus) NAME=pp-node-plus ;;
  *) echo "error: PP_NODE_EDITION must be node or plus" >&2; exit 2 ;;
esac
BUILD_DIR="${PP_NODE_BUILD_DIR:-${ROOT}/build-pp-node}"
OUT_DIR="${PP_NODE_OUT_DIR:-${ROOT}/dist/${NAME}}"
# Defaults come from CMakeLists.txt (the one place versions are set); CMake's
# project() needs a numeric PP_BROWSER_VERSION, so there is no "dev" fallback.
cmake_default() {
  sed -n "s/^set($1 \"\([^\"]*\)\".*/\1/p" "${ROOT}/CMakeLists.txt" | head -1
}
RELEASE_VERSION="${PP_BROWSER_RELEASE_VERSION:-${PP_BROWSER_VERSION:-$(cmake_default PP_BROWSER_RELEASE_VERSION)}}"
# CMakeLists.txt's release version is empty when it is the numeric one.
RELEASE_VERSION="${RELEASE_VERSION:-$(cmake_default PP_BROWSER_VERSION)}"
PROJECT_VERSION="${PP_BROWSER_VERSION:-${RELEASE_VERSION%%-*}}"
JOBS="${PP_NODE_JOBS:-$(nproc 2>/dev/null || echo 2)}"

usage() {
  cat <<EOF
Usage: $(basename "$0") [configure|build|package|all]

  configure  CMake configure (headless; Ubuntu 24.04)
  build      Build and strip pp-node (or pp-node-plus)
  package    Write tarball + Docker staging dir under dist/<name>/
  all        configure + build + package (default)

Environment:
  PP_NODE_EDITION     node (default: pp-node) or plus (pp-node-plus). Both build from one configured tree, so
                      configure once and run build / package per edition; each packages into its own OUT_DIR
  PP_BROWSER_VERSION / PP_BROWSER_RELEASE_VERSION  version strings (default:
                      the values in CMakeLists.txt)
  PP_NODE_BUILD_DIR   build tree (default: ./build-pp-node)
  PP_NODE_OUT_DIR     output dir (default: ./dist/pp-node, ./dist/pp-node-plus)
  PP_NODE_JOBS        parallel compile jobs
  PP_API_URL_PROD     prod backend origin compiled in (registration / directory URLs derive
                      from it; empty = none, so set it or the node's config / env for a release)
  PP_API_URL_STAGING  staging backend origin compiled in (empty = none)
  PP_API_URL_DEV      dev backend origin compiled in (empty = none)
  PP_RELAY_ID_PROD    relay id of the prod relay (relay:…, SIGNED_FORMATS § 3.3): the registration relay's with
                      the compiled URL (unset or empty = pending: not used; or registration.relay_id); its
                      certificate, the node's floor, is config/relay-certs/prod.cert
  PP_RELAY_ID_STAGING relay id of the staging relay (unset or empty = pending); config/relay-certs/staging.cert
  PP_RELAY_ID_DEV     relay id of the dev relay (unset or empty = pending); config/relay-certs/dev.cert
EOF
}

die() {
  echo "error: $*" >&2
  exit 1
}

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "missing command: $1"
}

do_configure() {
  need_cmd cmake
  need_cmd ninja
  mkdir -p "${BUILD_DIR}"
  cmake -B "${BUILD_DIR}" -S "${ROOT}" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DPP_BROWSER_HEADLESS=ON \
    -DPP_BROWSER_BUILD_TESTS=OFF \
    -DPP_BROWSER_COMPILER_CACHE=ON \
    -DPP_BROWSER_VERSION="${PROJECT_VERSION}" \
    -DPP_BROWSER_RELEASE_VERSION="${RELEASE_VERSION}" \
    -DPP_API_URL_PROD="${PP_API_URL_PROD:-}" \
    -DPP_API_URL_STAGING="${PP_API_URL_STAGING:-}" \
    -DPP_API_URL_DEV="${PP_API_URL_DEV:-}" \
    -DPP_RELAY_ID_PROD="${PP_RELAY_ID_PROD:-pending}" \
    -DPP_RELAY_ID_STAGING="${PP_RELAY_ID_STAGING:-pending}" \
    -DPP_RELAY_ID_DEV="${PP_RELAY_ID_DEV:-pending}"
}

do_build() {
  need_cmd cmake
  need_cmd strip
  [[ -f "${BUILD_DIR}/build.ninja" || -f "${BUILD_DIR}/CMakeCache.txt" ]] \
    || die "not configured; run configure first"
  # The post-build symbol checks (src/app/node/CMakeLists.txt) run on the unstripped binary, before the strip below.
  cmake --build "${BUILD_DIR}" --target "${NAME}" -j "${JOBS}"
  local bin="${BUILD_DIR}/src/app/node/${NAME}"
  [[ -x "${bin}" ]] || die "${NAME} binary missing: ${bin}"
  strip --strip-unneeded "${bin}"
  if command -v objdump >/dev/null 2>&1; then
    local max
    max="$(objdump -T "${bin}" 2>/dev/null | grep -oE 'GLIBC_[0-9.]+' | sort -Vu | tail -1 || true)"
    if [[ -n "${max}" ]]; then
      echo "${NAME} max GLIBC symbol: ${max} (runtime image: ubuntu:24.04)"
    fi
  fi
}

# packaging/pp-node/<file> as pp-node-plus's: its binary name where the file names pp-node's binary or its unit.
# Config, data and user stay pp-node's, so switching a node to pp-node-plus keeps them.
plus_copy() {
  local src="$1" dst="$2"
  sed -e 's#^COPY --chmod=755 pp-node /usr/local/bin/pp-node$#COPY --chmod=755 pp-node-plus /usr/local/bin/pp-node-plus#' \
    -e 's#^ENTRYPOINT \["/usr/local/bin/pp-node", #ENTRYPOINT ["/usr/local/bin/pp-node-plus", #' \
    -e 's#^ExecStart=/usr/bin/pp-node #ExecStart=/usr/bin/pp-node-plus #' \
    -e 's#^Description=Brief mesh node (pp-node)$#Description=Brief mesh node with add-on services (pp-node-plus)#' \
    "${src}" >"${dst}"
  if grep -qE '^(COPY --chmod=755 pp-node |ENTRYPOINT \["/usr/local/bin/pp-node"|ExecStart=/usr/bin/pp-node )' "${dst}"; then
    die "${src} still names pp-node's binary after the pp-node-plus rename; update plus_copy"
  fi
}

do_package() {
  local bin="${BUILD_DIR}/src/app/node/${NAME}"
  [[ -x "${bin}" ]] || die "${NAME} binary missing; run build first"

  local stage="${OUT_DIR}/stage"
  local docker_ctx="${OUT_DIR}/docker"
  local arch
  arch="$(uname -m)"
  case "${arch}" in
    x86_64) arch=amd64 ;;
    aarch64|arm64) arch=arm64 ;;
  esac

  rm -rf "${OUT_DIR}"
  local dir="${stage}/${NAME}-${RELEASE_VERSION}"
  mkdir -p "${dir}" "${docker_ctx}"

  cp "${bin}" "${dir}/${NAME}"
  cp "${ROOT}/packaging/pp-node/config.json.example" "${dir}/config.json.example"
  cp "${ROOT}/packaging/pp-node/99-pp-node-udp.conf" "${dir}/99-pp-node-udp.conf"
  if [[ "${NAME}" == pp-node ]]; then
    cp "${ROOT}/packaging/pp-node/pp-node.service" "${dir}/pp-node.service"
  else
    plus_copy "${ROOT}/packaging/pp-node/pp-node.service" "${dir}/${NAME}.service"
  fi

  local tarball="${OUT_DIR}/${NAME}-${RELEASE_VERSION}-linux-${arch}.tar.gz"
  tar -C "${stage}" -czf "${tarball}" "${NAME}-${RELEASE_VERSION}"

  cp "${bin}" "${docker_ctx}/${NAME}"
  cp "${ROOT}/packaging/pp-node/config.json.example" "${docker_ctx}/config.json.example"
  if [[ "${NAME}" == pp-node ]]; then
    cp "${ROOT}/packaging/pp-node/Dockerfile" "${docker_ctx}/Dockerfile"
  else
    plus_copy "${ROOT}/packaging/pp-node/Dockerfile" "${docker_ctx}/Dockerfile"
  fi

  ls -lh "${tarball}" "${docker_ctx}/${NAME}"
  echo "tarball=${tarball}"
  echo "docker_context=${docker_ctx}"
}

cmd="${1:-all}"
case "${cmd}" in
  -h|--help|help) usage ;;
  configure) do_configure ;;
  build) do_build ;;
  package) do_package ;;
  all)
    do_configure
    do_build
    do_package
    ;;
  *)
    usage
    die "unknown command: ${cmd}"
    ;;
esac
