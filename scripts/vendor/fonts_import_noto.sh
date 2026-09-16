#!/usr/bin/env bash
# Download Noto Sans CJK Regular (shared OTC) + Noto Color Emoji into assets/fonts/.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FONTS="${ROOT}/assets/fonts"
NOTO_CJK_REF="main"
NOTO_CJK_OTC_URL="https://github.com/notofonts/noto-cjk/raw/${NOTO_CJK_REF}/Sans/OTC/NotoSansCJK-Regular.ttc"
NOTO_COLOR_EMOJI_URL="https://github.com/googlefonts/noto-emoji/raw/main/fonts/NotoColorEmoji.ttf"
NOTO_COLOR_EMOJI_LICENSE_URL="https://raw.githubusercontent.com/googlefonts/noto-emoji/main/fonts/LICENSE"

mkdir -p "${FONTS}"

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

echo "==> Noto Sans CJK Regular (shared OTC: JP/KR/SC/TC/HK + Mono)"
download "${NOTO_CJK_OTC_URL}" "${FONTS}/NotoSansCJK-Regular.ttc"

# Remove superseded per-language OTFs (outlines were duplicated ~4x on disk).
for old in NotoSansCJKsc-Regular.otf NotoSansCJKjp-Regular.otf NotoSansCJKkr-Regular.otf NotoSansCJKtc-Regular.otf; do
  if [[ -f "${FONTS}/${old}" ]]; then
    echo "  remove superseded: ${old}"
    rm -f "${FONTS}/${old}"
  fi
done

echo "==> Noto Color Emoji (CBDT)"
download "${NOTO_COLOR_EMOJI_URL}" "${FONTS}/NotoColorEmoji.ttf"
if [[ ! -f "${FONTS}/NotoColorEmoji-LICENSE.txt" ]]; then
  curl -fsSL "${NOTO_COLOR_EMOJI_LICENSE_URL}" -o "${FONTS}/NotoColorEmoji-LICENSE.txt"
fi

# Keep monochrome face as secondary fallback for environments without color glyphs.
echo "==> Noto Emoji monochrome (secondary fallback)"
if [[ ! -f "${FONTS}/NotoEmoji-Regular.ttf" ]]; then
  echo "  missing NotoEmoji-Regular.ttf — copy from an RmlUi samples tree or restore from git history"
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
ls -lh "${FONTS}"/NotoSansCJK-Regular.ttc "${FONTS}"/NotoColorEmoji.ttf "${FONTS}"/NotoEmoji-Regular.ttf 2>/dev/null || true
