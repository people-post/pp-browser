#!/usr/bin/env bash
# Build a stripped Linux pp-node binary, tarball, and Docker build context.
# Intended for Ubuntu 24.04 (same family as packaging/pp-node/Dockerfile and
# release CI runners). See docs/ops/RELEASE.md.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${PP_NODE_BUILD_DIR:-${ROOT}/build-pp-node}"
OUT_DIR="${PP_NODE_OUT_DIR:-${ROOT}/dist/pp-node}"
RELEASE_VERSION="${PP_BROWSER_RELEASE_VERSION:-${PP_BROWSER_VERSION:-dev}}"
JOBS="${PP_NODE_JOBS:-$(nproc 2>/dev/null || echo 2)}"

usage() {
  cat <<EOF
Usage: $(basename "$0") [configure|build|package|all]

  configure  CMake configure (headless; Ubuntu 24.04)
  build      Build and strip pp-node
  package    Write tarball + Docker staging dir under dist/pp-node/
  all        configure + build + package (default)

Environment:
  PP_BROWSER_VERSION / PP_BROWSER_RELEASE_VERSION  version strings
  PP_NODE_BUILD_DIR   build tree (default: ./build-pp-node)
  PP_NODE_OUT_DIR     output dir (default: ./dist/pp-node)
  PP_NODE_JOBS        parallel compile jobs
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
    -DPP_BROWSER_VERSION="${PP_BROWSER_VERSION:-${RELEASE_VERSION%%-*}}" \
    -DPP_BROWSER_RELEASE_VERSION="${RELEASE_VERSION}"
}

do_build() {
  need_cmd cmake
  need_cmd strip
  [[ -f "${BUILD_DIR}/build.ninja" || -f "${BUILD_DIR}/CMakeCache.txt" ]] \
    || die "not configured; run configure first"
  cmake --build "${BUILD_DIR}" --target pp-node -j "${JOBS}"
  local bin="${BUILD_DIR}/src/app/node/pp-node"
  [[ -x "${bin}" ]] || die "pp-node binary missing: ${bin}"
  strip --strip-unneeded "${bin}"
  if command -v objdump >/dev/null 2>&1; then
    local max
    max="$(objdump -T "${bin}" 2>/dev/null | grep -oE 'GLIBC_[0-9.]+' | sort -Vu | tail -1 || true)"
    if [[ -n "${max}" ]]; then
      echo "pp-node max GLIBC symbol: ${max} (runtime image: ubuntu:24.04)"
    fi
  fi
}

do_package() {
  local bin="${BUILD_DIR}/src/app/node/pp-node"
  [[ -x "${bin}" ]] || die "pp-node binary missing; run build first"

  local stage="${OUT_DIR}/stage"
  local docker_ctx="${OUT_DIR}/docker"
  local arch
  arch="$(uname -m)"
  case "${arch}" in
    x86_64) arch=amd64 ;;
    aarch64|arm64) arch=arm64 ;;
  esac

  rm -rf "${OUT_DIR}"
  mkdir -p "${stage}/pp-node-${RELEASE_VERSION}" "${docker_ctx}"

  cp "${bin}" "${stage}/pp-node-${RELEASE_VERSION}/pp-node"
  cp "${ROOT}/packaging/pp-node/config.json.example" \
    "${stage}/pp-node-${RELEASE_VERSION}/config.json.example"
  cp "${ROOT}/packaging/pp-node/pp-node.service" \
    "${stage}/pp-node-${RELEASE_VERSION}/pp-node.service"

  local tarball="${OUT_DIR}/pp-node-${RELEASE_VERSION}-linux-${arch}.tar.gz"
  tar -C "${stage}" -czf "${tarball}" "pp-node-${RELEASE_VERSION}"

  cp "${bin}" "${docker_ctx}/pp-node"
  cp "${ROOT}/packaging/pp-node/config.json.example" "${docker_ctx}/config.json.example"
  cp "${ROOT}/packaging/pp-node/Dockerfile" "${docker_ctx}/Dockerfile"

  ls -lh "${tarball}" "${docker_ctx}/pp-node"
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
