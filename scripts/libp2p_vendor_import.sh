#!/usr/bin/env bash
# Populate third_party/ with cpp-libp2p dependencies (Hunter-pinned versions).
# Safe to re-run when bumping versions.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
THIRD_PARTY="${ROOT}/third_party"
TMP="${ROOT}/.libp2p_vendor_import_tmp"
PATCHES="${ROOT}/cmake/patches/libp2p"

# name|url|archive_type (git or tarball)
declare -A LIBP2P_REPOS=(
  [boringssl]="https://github.com/qdrvm/boringssl/archive/refs/tags/qdrvm1.zip|tarball"
  [lsquic]="https://github.com/qdrvm/lsquic/archive/refs/tags/v4.0.9-qdrvm-1.zip|tarball"
  [c-ares]="https://github.com/hunter-packages/c-ares/archive/v1.14.0-p0.tar.gz|tarball"
  [fmt]="https://github.com/fmtlib/fmt/archive/refs/tags/10.1.1.tar.gz|tarball"
  [yaml-cpp]="https://github.com/hunter-packages/yaml-cpp/archive/v0.6.2-0f9a586-p1.zip|tarball"
  [soralog]="https://github.com/qdrvm/soralog/archive/refs/tags/v0.2.5.tar.gz|tarball"
  [qtils]="https://github.com/qdrvm/qtils/archive/refs/tags/v0.1.1.tar.gz|tarball"
  [tsl_hat_trie]="https://github.com/masterjedy/hat-trie/archive/4fdfc75e75276185eed4b748ea09671601101b8e.tar.gz|tarball"
  [zlib]="https://github.com/qdrvm/zlib/archive/refs/tags/v1.3.0-p1.tar.gz|tarball"
  [googletest]="https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz|tarball"
)

import_tarball() {
  local name="$1"
  local url="$2"
  local dest="${THIRD_PARTY}/${name}"
  local work="${TMP}/${name}"
  local archive="${work}/archive"

  echo "==> ${name} (tarball)"
  rm -rf "${work}" "${dest}"
  mkdir -p "${work}"
  curl -fsSL "${url}" -o "${archive}"

  case "${url}" in
    *.zip) unzip -q "${archive}" -d "${work}/extract" ;;
    *) mkdir -p "${work}/extract" && tar -xf "${archive}" -C "${work}/extract" ;;
  esac

  local inner
  inner="$(find "${work}/extract" -mindepth 1 -maxdepth 1 -type d | head -1)"
  if [[ -z "${inner}" ]]; then
    echo "error: no extracted directory for ${name}" >&2
    exit 1
  fi

  mkdir -p "${dest}"
  # Drop upstream CI/hooks; soralog ships .github/aux/ which is invalid on Windows (reserved name).
  rsync -a --delete \
    --exclude='.git' \
    --exclude='.github' \
    --exclude='.githooks' \
    "${inner}/" "${dest}/"
  rm -rf "${work}"
}

apply_lsquic_patches() {
  local dest="${THIRD_PARTY}/lsquic"
  if [[ ! -d "${PATCHES}/lsquic" ]]; then
    return 0
  fi
  echo "    applying lsquic patches"
  for patch in "${PATCHES}/lsquic"/*.patch; do
    [[ -f "${patch}" ]] || continue
    # qdrvm 4.0.9-qdrvm-1 already ships lsquic_conn_ssl; applying again duplicates symbols.
    if [[ "$(basename "${patch}")" == "lsquic_conn_ssl.patch" ]]; then
      echo "      skipping $(basename "${patch}") (upstream already includes changes)"
      continue
    fi
    echo "      $(basename "${patch}")"
    patch -p1 -d "${dest}" < "${patch}" || {
      echo "warning: patch $(basename "${patch}") may already be applied" >&2
    }
  done
}

mkdir -p "${THIRD_PARTY}" "${TMP}"
json_entries=()

for name in "${!LIBP2P_REPOS[@]}"; do
  IFS='|' read -r url _kind <<< "${LIBP2P_REPOS[$name]}"
  import_tarball "${name}" "${url}"

  if [[ "${name}" == "lsquic" ]]; then
    apply_lsquic_patches
  fi

  json_entries+=("${name}|${url}")
done

rm -rf "${TMP}"

UPSTREAM="${THIRD_PARTY}/UPSTREAM.json"
export UPSTREAM
export json_blob="${json_entries[*]}"
python3 - <<PY
import json, os
from pathlib import Path

path = Path(os.environ["UPSTREAM"])
entries = os.environ.get("json_blob", "").split()

data = {}
if path.exists():
    data = json.loads(path.read_text())

libp2p = {}
for item in entries:
    if not item:
        continue
    name, url = item.split("|", 1)
    libp2p[name] = {"url": url}

data["libp2p_dependencies"] = libp2p
path.write_text(json.dumps(data, indent=2) + "\n")
print(f"Updated {path}")
PY

echo "Done. Review third_party/ and commit."
