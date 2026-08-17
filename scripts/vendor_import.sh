#!/usr/bin/env bash
# Populate third_party/ from upstream git tags. Safe to re-run when bumping versions.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
THIRD_PARTY="${ROOT}/third_party"
TMP="${ROOT}/.vendor_import_tmp"

# SQLite amalgamation (not a git repo).
SQLITE_AMALGAMATION_VERSION="3530300"
SQLITE_AMALGAMATION_YEAR="2026"

declare -A REPOS=(
  [freetype]="https://github.com/freetype/freetype.git|VER-2-13-3"
  [nlohmann_json]="https://github.com/nlohmann/json.git|v3.11.3"
  [curl]="https://github.com/curl/curl.git|curl-8_11_1"
  [sdl3]="https://github.com/libsdl-org/SDL.git|release-3.2.8"
  [sdl3_image]="https://github.com/libsdl-org/SDL_image.git|release-3.2.4"
  [lunasvg]="https://github.com/sammycage/lunasvg.git|v3.5.0"
  [libsodium]="https://github.com/jedisct1/libsodium.git|1.0.20-RELEASE"
  [mlkem-native]="https://github.com/pq-code-package/mlkem-native.git|v2.0.0"
  [mldsa-native]="https://github.com/pq-code-package/mldsa-native.git|v2.0.0"
  [harfbuzz]="https://github.com/harfbuzz/harfbuzz.git|9.0.0"
  [opus]="https://github.com/xiph/opus.git|v1.5.2"
)

DEFAULT_ORDER=(freetype nlohmann_json curl sdl3 sdl3_image lunasvg libsodium mlkem-native mldsa-native harfbuzz opus sqlite)

# PQCP libs: vendor lean mlkem/ or mldsa/ only + preserve pp-browser CMake/README overlays.
import_pqcp_lean() {
  local name="$1"
  local url="$2"
  local tag="$3"
  local dest="${THIRD_PARTY}/${name}"
  local clone_dir="${TMP}/${name}"
  local subdir
  if [[ "${name}" == "mlkem-native" ]]; then
    subdir="mlkem"
  else
    subdir="mldsa"
  fi

  echo "==> ${name} @ ${tag} (lean ${subdir}/)"
  preserve_pp_cmake "${name}"
  local readme_backup="${TMP}/${name}_README.pp-browser.md"
  if [[ -f "${dest}/README.pp-browser.md" ]]; then
    cp "${dest}/README.pp-browser.md" "${readme_backup}"
  fi

  git clone --depth 1 --branch "${tag}" "${url}" "${clone_dir}"
  local commit
  commit="$(git -C "${clone_dir}" rev-parse HEAD)"

  rm -rf "${dest}"
  mkdir -p "${dest}"
  cp -a "${clone_dir}/${subdir}" "${dest}/${subdir}"
  cp "${clone_dir}/LICENSE" "${dest}/LICENSE"
  echo "${commit}" > "${dest}/UPSTREAM_COMMIT"
  find "${dest}" -name '.git' -exec rm -rf {} + 2>/dev/null || true
  restore_pp_cmake "${name}"
  if [[ -f "${readme_backup}" ]]; then
    cp "${readme_backup}" "${dest}/README.pp-browser.md"
  fi

  python3 - "${dest}" "${name}" <<'PY'
import sys
from pathlib import Path
dest, name = Path(sys.argv[1]), sys.argv[2]
mlkem_rng = '''#define MLK_CONFIG_CUSTOM_RANDOMBYTES
#if !defined(__ASSEMBLER__)
#include <stdint.h>
#include <sodium.h>
#include "src/sys.h"
static MLK_INLINE int mlk_randombytes(uint8_t *ptr, size_t len)
{
  if (ptr == NULL && len != 0) {
    return -1;
  }
  if (sodium_init() < 0) {
    return -1;
  }
  if (len == 0) {
    return 0;
  }
  randombytes_buf(ptr, len);
  return 0;
}
#endif'''
mldsa_rng = '''#define MLD_CONFIG_CUSTOM_RANDOMBYTES
#if !defined(__ASSEMBLER__)
#include <stdint.h>
#include <sodium.h>
#include "src/sys.h"
static MLD_INLINE int mld_randombytes(uint8_t *ptr, size_t len)
{
  if (ptr == NULL && len != 0) {
    return -1;
  }
  if (sodium_init() < 0) {
    return -1;
  }
  if (len == 0) {
    return 0;
  }
  randombytes_buf(ptr, len);
  return 0;
}
#endif'''
mlkem_rng_old = '''/* #define MLK_CONFIG_CUSTOM_RANDOMBYTES
   #if !defined(__ASSEMBLER__)
   #include <stdint.h>
   #include "src/sys.h"
   static MLK_INLINE int mlk_randombytes(uint8_t *ptr, size_t len)
   {
       ... your implementation ...
       return 0;
   }
   #endif
*/'''
mldsa_rng_old = '''/* #define MLD_CONFIG_CUSTOM_RANDOMBYTES
   #if !defined(__ASSEMBLER__)
   #include <stdint.h>
   #include "src/src.h"
   static MLD_INLINE int mld_randombytes(uint8_t *ptr, size_t len)
   {
       ... your implementation ...
       return 0;
   }
   #endif
*/'''
if name == "mlkem-native":
    p = dest / "mlkem" / "mlkem_native_config.h"
    text = p.read_text()
    text = text.replace(
        "#define MLK_CONFIG_PARAMETER_SET \\\n  768 /* Change this for different security strengths */",
        "#define MLK_CONFIG_PARAMETER_SET \\\n  768 /* pp-browser: ML-KEM-768 */",
    )
    text = text.replace(
        "#if !defined(MLK_CONFIG_NAMESPACE_PREFIX)\n#define MLK_CONFIG_NAMESPACE_PREFIX MLK_DEFAULT_NAMESPACE_PREFIX\n#endif",
        "#if !defined(MLK_CONFIG_NAMESPACE_PREFIX)\n#define MLK_CONFIG_NAMESPACE_PREFIX mlkem\n#endif",
    )
    text = text.replace(
        "/* #define MLK_CONFIG_INTERNAL_API_QUALIFIER */",
        "#define MLK_CONFIG_INTERNAL_API_QUALIFIER static",
        1,
    )
    if mlkem_rng_old not in text:
        raise SystemExit("mlkem CUSTOM_RANDOMBYTES template not found")
    text = text.replace(mlkem_rng_old, mlkem_rng, 1)
    p.write_text(text)
else:
    p = dest / "mldsa" / "mldsa_native_config.h"
    text = p.read_text()
    text = text.replace(
        "#define MLD_CONFIG_PARAMETER_SET \\\n  44 /* Change this for different security strengths */",
        "#define MLD_CONFIG_PARAMETER_SET \\\n  65 /* pp-browser: ML-DSA-65 */",
    )
    text = text.replace(
        "#if !defined(MLD_CONFIG_NAMESPACE_PREFIX)\n#define MLD_CONFIG_NAMESPACE_PREFIX MLD_DEFAULT_NAMESPACE_PREFIX\n#endif",
        "#if !defined(MLD_CONFIG_NAMESPACE_PREFIX)\n#define MLD_CONFIG_NAMESPACE_PREFIX mldsa\n#endif",
    )
    text = text.replace(
        "/* #define MLD_CONFIG_INTERNAL_API_QUALIFIER */",
        "#define MLD_CONFIG_INTERNAL_API_QUALIFIER static",
        1,
    )
    if mldsa_rng_old not in text:
        raise SystemExit("mldsa CUSTOM_RANDOMBYTES template not found")
    text = text.replace(mldsa_rng_old, mldsa_rng, 1)
    p.write_text(text)
print(f"patched {name} config")
PY

  python3 - "${PATCH_JSON}" "${name}" "${url}" "${tag}" "${commit}" <<'PY'
import json, sys
from pathlib import Path
path, name, url, tag, commit = sys.argv[1:6]
data = json.loads(Path(path).read_text())
data[name] = {
    "repository": url,
    "tag": tag,
    "commit": commit,
    "notes": f"Vendored lean tree only; see README.pp-browser.md",
}
Path(path).write_text(json.dumps(data))
PY
}

import_sdl3_image_externals() {
  local image_root="${THIRD_PARTY}/sdl3_image"
  local gitmodules="${image_root}/.gitmodules"
  if [[ ! -f "${gitmodules}" ]]; then
    echo "error: missing ${gitmodules}" >&2
    exit 1
  fi

  echo "==> sdl3_image external/ codec sources"
  mkdir -p "${image_root}/external"

  cd "${image_root}"
  while true; do
    read -r module || break
    read -r line; set -- ${line}; local path=$3
    read -r line; set -- ${line}; local url=$3
    read -r line; set -- ${line}; local branch=$3

    local name="${path##*/}"
    local dest="${image_root}/${path}"
    local clone_tmp="${TMP}/sdl3_image_${name}"
    echo "    ${name} @ ${branch}"
    rm -rf "${clone_tmp}" "${dest}"
    git clone --depth 1 --filter=blob:none --branch "${branch}" --recursive \
      "${url}" "${clone_tmp}"
    local commit
    commit="$(git -C "${clone_tmp}" rev-parse HEAD)"
    mkdir -p "${dest}"
    rsync -a --delete --exclude='.git' "${clone_tmp}/" "${dest}/"
    find "${dest}" -name '.git' -exec rm -rf {} + 2>/dev/null || true
    rm -rf "${clone_tmp}"
    external_entries+=("    \"${name}\": {
      \"repository\": \"${url}\",
      \"branch\": \"${branch}\",
      \"commit\": \"${commit}\"
    }")
  done < "${gitmodules}"
  cd "${ROOT}"
}

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
external_entries=()

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

  if [[ "${name}" == "mlkem-native" || "${name}" == "mldsa-native" ]]; then
    import_pqcp_lean "${name}" "${url}" "${tag}"
    continue
  fi

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

  if [[ "${name}" == "libsodium" ]]; then
    python3 "${ROOT}/scripts/generate_libsodium_cmake.py" "${dest}"
  fi

  python3 - "${PATCH_JSON}" "${name}" "${url}" "${tag}" "${commit}" <<'PY'
import json, sys
from pathlib import Path
path, name, url, tag, commit = sys.argv[1:6]
data = json.loads(Path(path).read_text())
data[name] = {"repository": url, "tag": tag, "commit": commit}
Path(path).write_text(json.dumps(data))
PY

  if [[ "${name}" == "sdl3_image" ]]; then
    import_sdl3_image_externals
  fi
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

if [[ ${#external_entries[@]} -gt 0 ]]; then
  export UPSTREAM
  export EXTERNAL_BLOB="${external_entries[*]}"
  python3 - <<'PY'
import json
import os
from pathlib import Path

path = Path(os.environ["UPSTREAM"])
data = json.loads(path.read_text())
externals = data.setdefault("sdl3_image_externals", {})
for chunk in os.environ.get("EXTERNAL_BLOB", "").split("    \""):
    chunk = chunk.strip()
    if not chunk:
        continue
    name = chunk.split("\"", 1)[0]
    body = "{" + chunk[name.__len__() + 1 :]
    _, _, rest = body.partition(":")
    externals[name] = json.loads(rest.strip().rstrip(","))
path.write_text(json.dumps(data, indent=2) + "\n")
PY
fi

rm -rf "${TMP}"

echo "Done. Review changes and commit third_party/."
