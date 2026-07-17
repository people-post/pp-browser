#!/usr/bin/env bash
# Import upstream RmlUi test trees into the hard fork under src/render/fork/.
# Safe to re-run when bumping the fork version.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FORK="${ROOT}/src/render/fork"
TMP="${ROOT}/.rmlui_tests_import_tmp"

RMLUI_REPO="https://github.com/mikke89/RmlUi.git"
RMLUI_VERSION="6.2"
RMLUI_TAG="${RMLUI_VERSION}"

echo "==> Importing RmlUi ${RMLUI_TAG} test trees into ${FORK}"

rm -rf "${TMP}"
mkdir -p "${TMP}"

git clone --depth 1 --branch "${RMLUI_TAG}" "${RMLUI_REPO}" "${TMP}/RmlUi"

copy_tree() {
  local src_rel="$1"
  local dest_rel="$2"
  local src="${TMP}/RmlUi/${src_rel}"
  local dest="${FORK}/${dest_rel}"

  if [[ ! -d "${src}" ]]; then
    echo "error: missing upstream path ${src_rel}" >&2
    exit 1
  fi

  echo "    ${src_rel} -> ${dest_rel}"
  rm -rf "${dest}"
  mkdir -p "$(dirname "${dest}")"
  if command -v rsync >/dev/null 2>&1; then
    rsync -a --delete \
      --exclude='.git' \
      --exclude='.github' \
      "${src}/" "${dest}/"
  else
    mkdir -p "${dest}"
    cp -a "${src}/." "${dest}/"
    rm -rf "${dest}/.git" "${dest}/.github"
  fi
}

copy_tree "Tests" "Tests"
copy_tree "Samples/shell" "Samples/shell"
copy_tree "Samples/assets" "Samples/assets"

COMMIT_SHA="$(git -C "${TMP}/RmlUi" rev-parse HEAD)"

cat > "${FORK}/UPSTREAM.json" <<EOF
{
  "repository": "${RMLUI_REPO}",
  "version": "${RMLUI_VERSION}",
  "tag": "${RMLUI_TAG}",
  "commit": "${COMMIT_SHA}",
  "imported_paths": [
    "Tests",
    "Samples/shell",
    "Samples/assets"
  ],
  "import_script": "scripts/rmlui_tests_import.sh"
}
EOF

rm -rf "${TMP}"
echo "==> Done (commit ${COMMIT_SHA:0:12})"
