#!/usr/bin/env bash
# Populate third_party/ from upstream git tags. Safe to re-run when bumping versions.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# FreeType / HarfBuzz / LunaSVG / SDL3 / SDL3_image live in people-post/pp-cpp-ui.
THIRD_PARTY="${ROOT}/third_party"
TMP="${ROOT}/.vendor_import_tmp"

# SQLite amalgamation (not a git repo).
SQLITE_AMALGAMATION_VERSION="3530300"
SQLITE_AMALGAMATION_YEAR="2026"

declare -A REPOS=(
  [curl]="https://github.com/curl/curl.git|curl-8_11_1"
  [opus]="https://github.com/xiph/opus.git|v1.5.2"
)

DEFAULT_ORDER=(curl opus sqlite)

# PQ natives (libsodium / mlkem / mldsa) live in people-post/pp-cpp-crypto.
# SDL3 / SDL3_image live in people-post/pp-cpp-ui.


import_sqlite_amalgamation() {
  local dest="${THIRD_PARTY}/sqlite"
  local url="https://www.sqlite.org/${SQLITE_AMALGAMATION_YEAR}/sqlite-amalgamation-${SQLITE_AMALGAMATION_VERSION}.zip"
  local archive="${TMP}/sqlite-amalgamation.zip"
  local extract="${TMP}/sqlite-extract"

  echo "==> sqlite amalgamation ${SQLITE_AMALGAMATION_VERSION}"
  mkdir -p "${dest}"
  if [[ -f "${dest}/CMakeLists.txt" ]]; then
    cp "${dest}/CMakeLists.txt" "${TMP}/sqlite_CMakeLists.txt"
  fi

  rm -rf "${extract}" "${archive}"
  curl -fsSL "${url}" -o "${archive}"
  unzip -q "${archive}" -d "${extract}"
  local inner="${extract}/sqlite-amalgamation-${SQLITE_AMALGAMATION_VERSION}"
  if [[ ! -d "${inner}" ]]; then
    echo "error: unexpected sqlite amalgamation layout" >&2
    exit 1
  fi

  rm -f "${dest}/sqlite3.c" "${dest}/sqlite3.h" "${dest}/sqlite3ext.h" "${dest}/shell.c"
  cp "${inner}/sqlite3.c" "${inner}/sqlite3.h" "${inner}/sqlite3ext.h" "${dest}/"

  if [[ -f "${TMP}/sqlite_CMakeLists.txt" ]]; then
    cp "${TMP}/sqlite_CMakeLists.txt" "${dest}/CMakeLists.txt"
  fi

  python3 - "${PATCH_JSON}" <<PY
import json, sys
from pathlib import Path
path = Path(sys.argv[1])
data = json.loads(path.read_text())
data["sqlite"] = {
    "source": "${url}",
    "amalgamation_version": "${SQLITE_AMALGAMATION_VERSION}",
}
path.write_text(json.dumps(data))
PY
}

preserve_pp_cmake() {
  local name="$1"
  local dest="${THIRD_PARTY}/${name}"
  local backup="${TMP}/${name}_CMakeLists.txt"
  if [[ -f "${dest}/CMakeLists.txt" ]]; then
    cp "${dest}/CMakeLists.txt" "${backup}"
  else
    rm -f "${backup}"
  fi
}

restore_pp_cmake() {
  local name="$1"
  local dest="${THIRD_PARTY}/${name}"
  local backup="${TMP}/${name}_CMakeLists.txt"
  if [[ -f "${backup}" ]]; then
    cp "${backup}" "${dest}/CMakeLists.txt"
  fi
}

mkdir -p "${THIRD_PARTY}"
rm -rf "${TMP}"
mkdir -p "${TMP}"

PATCH_JSON="${TMP}/upstream_patch.json"
echo "{}" > "${PATCH_JSON}"

if [[ $# -gt 0 ]]; then
  ORDER=("$@")
else
  ORDER=("${DEFAULT_ORDER[@]}")
fi

for name in "${ORDER[@]}"; do
  if [[ "${name}" == "sqlite" ]]; then
    import_sqlite_amalgamation
    continue
  fi

  if [[ -z "${REPOS[$name]+x}" ]]; then
    echo "error: unknown vendored dependency '${name}'" >&2
    exit 1
  fi

  IFS='|' read -r url tag <<< "${REPOS[$name]}"

  dest="${THIRD_PARTY}/${name}"
  clone_dir="${TMP}/${name}"

  echo "==> ${name} @ ${tag}"
  preserve_pp_cmake "${name}"

  if [[ "${name}" == "lunasvg" ]]; then
    git clone --depth 1 --branch "${tag}" --recursive "${url}" "${clone_dir}"
  else
    git clone --depth 1 --branch "${tag}" "${url}" "${clone_dir}"
  fi
  commit="$(git -C "${clone_dir}" rev-parse HEAD)"

  rm -rf "${dest}"
  mkdir -p "${dest}"
  if command -v rsync >/dev/null 2>&1; then
    rsync -a --delete --exclude='.git' "${clone_dir}/" "${dest}/"
  else
    cp -a "${clone_dir}/." "${dest}/"
    find "${dest}" -name '.git' -exec rm -rf {} + 2>/dev/null || true
  fi
  find "${dest}" -name '.git' -exec rm -rf {} + 2>/dev/null || true
  restore_pp_cmake "${name}"

  python3 - "${PATCH_JSON}" "${name}" "${url}" "${tag}" "${commit}" <<'PY'
import json, sys
from pathlib import Path
path, name, url, tag, commit = sys.argv[1:6]
data = json.loads(Path(path).read_text())
data[name] = {"repository": url, "tag": tag, "commit": commit}
Path(path).write_text(json.dumps(data))
PY

done

UPSTREAM="${THIRD_PARTY}/UPSTREAM.json"
python3 - "${UPSTREAM}" "${PATCH_JSON}" <<'PY'
import json
import sys
from pathlib import Path

upstream_path = Path(sys.argv[1])
patch_path = Path(sys.argv[2])

data = {}
if upstream_path.exists():
    data = json.loads(upstream_path.read_text())

patch = json.loads(patch_path.read_text())
data.update(patch)

upstream_path.write_text(json.dumps(data, indent=2) + "\n")
print(f"Updated {upstream_path}")
PY


rm -rf "${TMP}"

echo "Done. Review changes and commit third_party/."
