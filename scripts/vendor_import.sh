#!/usr/bin/env bash
# Populate third_party/ from upstream git tags. Safe to re-run when bumping versions.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
THIRD_PARTY="${ROOT}/third_party"
TMP="${ROOT}/.vendor_import_tmp"

declare -A REPOS=(
  [freetype]="https://github.com/freetype/freetype.git|VER-2-13-3"
  [nlohmann_json]="https://github.com/nlohmann/json.git|v3.11.3"
  [curl]="https://github.com/curl/curl.git|curl-8_11_1"
  [sdl3]="https://github.com/libsdl-org/SDL.git|release-3.2.8"
  [sdl3_image]="https://github.com/libsdl-org/SDL_image.git|release-3.2.4"
)

import_sdl3_image_externals() {
  local image_root="${THIRD_PARTY}/sdl3_image"
  local gitmodules="${image_root}/.gitmodules"
  if [[ ! -f "${gitmodules}" ]]; then
    echo "error: missing ${gitmodules}" >&2
    exit 1
  fi

  echo "==> sdl3_image external/ submodules"
  mkdir -p "${image_root}/external"

  cd "${image_root}"
  while true; do
    read -r module || break
    read -r line; set -- ${line}; local path=$3
    read -r line; set -- ${line}; local url=$3
    read -r line; set -- ${line}; local branch=$3

    local name="${path##*/}"
    local dest="${image_root}/${path}"
    echo "    ${name} @ ${branch}"
    rm -rf "${dest}"
    git clone --depth 1 --filter=blob:none --branch "${branch}" --recursive \
      "${url}" "${dest}"
    external_entries+=("    \"${name}\": {
      \"repository\": \"${url}\",
      \"branch\": \"${branch}\",
      \"commit\": \"$(git -C "${dest}" rev-parse HEAD)\"
    }")
  done < "${gitmodules}"
  cd "${ROOT}"
}

mkdir -p "${THIRD_PARTY}"
rm -rf "${TMP}"
mkdir -p "${TMP}"

json_entries=()
external_entries=()

for name in freetype nlohmann_json curl sdl3 sdl3_image; do
  IFS='|' read -r url tag <<< "${REPOS[$name]}"
  dest="${THIRD_PARTY}/${name}"
  clone_dir="${TMP}/${name}"

  echo "==> ${name} @ ${tag}"
  git clone --depth 1 --branch "${tag}" "${url}" "${clone_dir}"
  commit="$(git -C "${clone_dir}" rev-parse HEAD)"

  rm -rf "${dest}"
  mkdir -p "${dest}"
  rsync -a --delete --exclude='.git' "${clone_dir}/" "${dest}/"

  json_entries+=("  \"${name}\": {
    \"repository\": \"${url}\",
    \"tag\": \"${tag}\",
    \"commit\": \"${commit}\"
  }")

  if [[ "${name}" == "sdl3_image" ]]; then
    import_sdl3_image_externals
  fi
done

rm -rf "${TMP}"

UPSTREAM="${THIRD_PARTY}/UPSTREAM.json"
{
  echo "{"
  for i in "${!json_entries[@]}"; do
    if [[ $i -gt 0 ]]; then echo ","; fi
    echo -n "${json_entries[$i]}"
  done
  if [[ ${#external_entries[@]} -gt 0 ]]; then
    echo ","
    echo "  \"sdl3_image_externals\": {"
    for i in "${!external_entries[@]}"; do
      if [[ $i -gt 0 ]]; then echo ","; fi
      echo -n "${external_entries[$i]}"
    done
    echo
    echo "  }"
  fi
  echo "}"
} > "${UPSTREAM}"

echo "Wrote ${UPSTREAM}"
echo "Done. Review changes and commit third_party/."
