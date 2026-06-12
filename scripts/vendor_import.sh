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

mkdir -p "${THIRD_PARTY}"
rm -rf "${TMP}"
mkdir -p "${TMP}"

json_entries=()

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
done

rm -rf "${TMP}"

UPSTREAM="${THIRD_PARTY}/UPSTREAM.json"
{
  echo "{"
  for i in "${!json_entries[@]}"; do
    if [[ $i -gt 0 ]]; then echo ","; fi
    echo -n "${json_entries[$i]}"
  done
  echo
  echo "}"
} > "${UPSTREAM}"

echo "Wrote ${UPSTREAM}"
echo "Done. Review changes and commit third_party/."
