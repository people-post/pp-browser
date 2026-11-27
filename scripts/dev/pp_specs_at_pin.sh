#!/usr/bin/env bash
# Clone pp-specs at the tag cmake/PpPins.cmake pins, into <dir> (shallow), for a check that reads its vectors without
# a CMake configure (CI's lint job: scripts/check/check_relay_ids.sh --self-test). Prints <dir>.
#
#   scripts/dev/pp_specs_at_pin.sh <dir>
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
dir="${1:?usage: $0 <dir>}"
tag="$(sed -n 's/^pp_pin(pp-specs[[:space:]]*TAG[[:space:]]*\([^)[:space:]]*\)).*/\1/p' "${root}/cmake/PpPins.cmake")"
[[ -n "${tag}" ]] || { echo "no pp-specs pin in cmake/PpPins.cmake" >&2; exit 1; }
rm -rf "${dir}"
git -c advice.detachedHead=false clone -q --depth 1 --branch "${tag}" https://github.com/people-post/pp-specs.git "${dir}"
echo "${dir}"
