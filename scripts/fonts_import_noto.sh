#!/usr/bin/env bash
# Download Noto Sans CJK Regular + Noto Emoji into assets/fonts/.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FONTS="${ROOT}/assets/fonts"
TMP="${ROOT}/.fonts_import_tmp"
NOTO_CJK_REF="main"
NOTO_CJK_BASE="https://github.com/notofonts/noto-cjk/raw/${NOTO_CJK_REF}/Sans/OTF"

mkdir -p "${FONTS}" "${TMP}"

download() {
  local url="$1"
  local dest="$2"
  if [[ -f "${dest}" ]]; then
    echo "  skip (exists): $(basename "${dest}")"
    return 0
  fi
  echo "  fetch: $(basename "${dest}")"
  curl -fsSL "${url}" -o "${dest}.part"
  mv "${dest}.part" "${dest}"
}

echo "==> Noto Sans CJK Regular (SC/JP/KR/TC)"
download "${NOTO_CJK_BASE}/SimplifiedChinese/NotoSansCJKsc-Regular.otf" "${FONTS}/NotoSansCJKsc-Regular.otf"
download "${NOTO_CJK_BASE}/Japanese/NotoSansCJKjp-Regular.otf" "${FONTS}/NotoSansCJKjp-Regular.otf"
download "${NOTO_CJK_BASE}/Korean/NotoSansCJKkr-Regular.otf" "${FONTS}/NotoSansCJKkr-Regular.otf"
download "${NOTO_CJK_BASE}/TraditionalChinese/NotoSansCJKtc-Regular.otf" "${FONTS}/NotoSansCJKtc-Regular.otf"

echo "==> Noto Emoji (from RmlUi samples)"
if [[ ! -f "${FONTS}/NotoEmoji-Regular.ttf" ]]; then
  cp "${ROOT}/src/render/fork/Samples/assets/NotoEmoji-Regular.ttf" "${FONTS}/NotoEmoji-Regular.ttf"
fi

if [[ ! -f "${FONTS}/NotoCJK-LICENSE.txt" ]]; then
  cat > "${FONTS}/NotoCJK-LICENSE.txt" <<'EOF'
Noto Sans CJK and Noto Emoji are licensed under the SIL Open Font License 1.1.
See https://github.com/notofonts/noto-cjk and NotoEmoji-LICENSE.txt in RmlUi samples.
EOF
fi

if [[ -f "${FONTS}/NotoSansSC-Regular.subset.ttf" ]]; then
  echo "==> Removing superseded UI subset font"
  rm -f "${FONTS}/NotoSansSC-Regular.subset.ttf"
fi

echo "==> Done. Font sizes:"
ls -lh "${FONTS}"/NotoSansCJK*.otf "${FONTS}"/NotoEmoji-Regular.ttf 2>/dev/null || true
