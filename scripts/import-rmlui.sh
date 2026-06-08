#!/usr/bin/env bash
# Re-import RmlUi upstream into thirdparty/rmlui/ (hard fork sync helper).
set -euo pipefail

TAG="${1:-6.2}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${ROOT}/thirdparty/rmlui"
TMP="$(mktemp -d)"

cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

git clone --depth 1 --branch "$TAG" https://github.com/mikke89/RmlUi.git "$TMP"
COMMIT="$(git -C "$TMP" rev-parse HEAD)"

rsync -a --delete \
  --exclude='.git' \
  --exclude='Samples' \
  --exclude='Tests' \
  --exclude='.github' \
  --exclude='CMakePresets.json' \
  --exclude='UPSTREAM.json' \
  "$TMP/" "$DEST/"

IMPORTED="$(date -u +%Y-%m-%d)"
cat > "${DEST}/UPSTREAM.json" <<EOF
{
  "name": "RmlUi",
  "url": "https://github.com/mikke89/RmlUi",
  "tag": "${TAG}",
  "commit": "${COMMIT}",
  "imported": "${IMPORTED}",
  "license": "MIT"
}
EOF

echo "Imported RmlUi ${TAG} (${COMMIT}) into ${DEST}"
echo "Re-copy SDL_GL3 backend files into src/render/backends/ if upstream Backends/ changed."
echo "Re-apply fork patch: wrap add_subdirectory(\"Samples\") in if(RMLUI_SAMPLES) in thirdparty/rmlui/CMakeLists.txt"
