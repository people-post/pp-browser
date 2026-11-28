#!/usr/bin/env bash
# The default pp-node stays small (projects/node-relay NR001): its stripped binary, and what its image adds to
# ubuntu:24.04, each under a budget. pp-node-plus has no budget. Raising one is a deliberate edit here, in the PR that
# needs it, with the reason in its description.
#
# Usage: check_node_size.sh <stripped pp-node> [<pp-node image>]
#   CI's node-editions job (.github/workflows/build.yml) runs it on scripts/platform/pp_node_package_linux.sh's output.
set -euo pipefail

# Measured 2026-11-27 (linux amd64, Release, ledger on): binary 12,269,032 bytes; image over its base 15,198,666 bytes.
BINARY_BUDGET=$((14 * 1024 * 1024))
IMAGE_OVER_BASE_BUDGET=$((18 * 1024 * 1024))
BASE_IMAGE=ubuntu:24.04

[[ $# -ge 1 && $# -le 2 ]] || { echo "usage: $(basename "$0") <stripped pp-node> [<pp-node image>]" >&2; exit 2; }
bin="$1"
image="${2:-}"
[[ -f "${bin}" ]] || { echo "error: no binary: ${bin}" >&2; exit 2; }

mib() { awk -v b="$1" 'BEGIN { printf "%.1f MiB", b / 1048576 }'; }
fail=0

if file "${bin}" 2>/dev/null | grep -q 'not stripped'; then
  echo "error: ${bin} is not stripped (run the package script's build step)" >&2
  exit 2
fi
size="$(stat -c %s "${bin}")"
echo "pp-node binary: ${size} bytes ($(mib "${size}")), budget $(mib "${BINARY_BUDGET}")"
if (( size > BINARY_BUDGET )); then
  echo "FAIL: the default pp-node binary is over its budget" >&2
  fail=1
fi

if [[ -n "${image}" ]]; then
  docker image inspect "${BASE_IMAGE}" >/dev/null 2>&1 || docker pull -q "${BASE_IMAGE}" >/dev/null
  total="$(docker image inspect -f '{{.Size}}' "${image}")"
  base="$(docker image inspect -f '{{.Size}}' "${BASE_IMAGE}")"
  over=$((total - base))
  echo "pp-node image: ${total} bytes, ${over} over ${BASE_IMAGE} ($(mib "${over}")), budget $(mib "${IMAGE_OVER_BASE_BUDGET}")"
  if (( over > IMAGE_OVER_BASE_BUDGET )); then
    echo "FAIL: the default pp-node image adds more than its budget to ${BASE_IMAGE}" >&2
    fail=1
  fi
fi

exit "${fail}"
