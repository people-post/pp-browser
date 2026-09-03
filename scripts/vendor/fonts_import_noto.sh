#!/usr/bin/env bash
# Download Noto Sans CJK Regular + Noto Color Emoji into assets/fonts/.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FONTS="${ROOT}/assets/fonts"
TMP="${ROOT}/.fonts_import_tmp"
NOTO_CJK_REF="main"
NOTO_CJK_BASE="https://github.com/notofonts/noto-cjk/raw/${NOTO_CJK_REF}/Sans/OTF"
NOTO_COLOR_EMOJI_URL="https://github.com/googlefonts/noto-emoji/raw/main/fonts/NotoColorEmoji.ttf"
NOTO_COLOR_EMOJI_LICENSE_URL="https://raw.githubusercontent.com/googlefonts/noto-emoji/main/fonts/LICENSE"

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

echo "==> Noto Color Emoji (CBDT)"
download "${NOTO_COLOR_EMOJI_URL}" "${FONTS}/NotoColorEmoji.ttf"
if [[ ! -f "${FONTS}/NotoColorEmoji-LICENSE.txt" ]]; then
  curl -fsSL "${NOTO_COLOR_EMOJI_LICENSE_URL}" -o "${FONTS}/NotoColorEmoji-LICENSE.txt"
fi

# Keep monochrome face as secondary fallback for environments without color glyphs.
echo "==> Noto Emoji monochrome (secondary fallback)"
if [[ ! -f "${FONTS}/NotoEmoji-Regular.ttf" ]]; then
  cp "${ROOT}/src/lib/rmlui/Samples/assets/NotoEmoji-Regular.ttf" "${FONTS}/NotoEmoji-Regular.ttf"
fi

if [[ ! -f "${FONTS}/NotoCJK-LICENSE.txt" ]]; then
  cat > "${FONTS}/NotoCJK-LICENSE.txt" <<'EOF'
Noto Sans CJK and Noto Emoji are licensed under the SIL Open Font License 1.1.
See https://github.com/notofonts/noto-cjk and NotoColorEmoji-LICENSE.txt / NotoEmoji-LICENSE.txt.
EOF
fi

if [[ ! -f "${FONTS}/NotoEmoji-LICENSE.txt" ]]; then
  cat > "${FONTS}/NotoEmoji-LICENSE.txt" <<'EOF'
Noto Emoji (monochrome) is licensed under the SIL Open Font License 1.1.
See https://github.com/googlefonts/noto-emoji
EOF
fi

if [[ -f "${FONTS}/NotoSansSC-Regular.subset.ttf" ]]; then
  echo "==> Removing superseded UI subset font"
  rm -f "${FONTS}/NotoSansSC-Regular.subset.ttf"
fi

echo "==> Done. Font sizes:"
ls -lh "${FONTS}"/NotoSansCJK*.otf "${FONTS}"/NotoColorEmoji.ttf "${FONTS}"/NotoEmoji-Regular.ttf 2>/dev/null || true
