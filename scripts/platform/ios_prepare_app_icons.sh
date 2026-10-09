#!/usr/bin/env bash
# Generate AppIcon.appiconset + compile Assets.car for the iOS bundle.
# Merges actool partial Info.plist (CFBundleIcons*) and copies loose PNGs.
# Usage: ios_prepare_app_icons.sh <output_app_bundle_dir>
# Source master: assets/branding/app-icon.png (1024²)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MASTER="${ROOT}/assets/branding/app-icon.png"
APP_DIR="${1:?app bundle directory required}"
DEPLOYMENT_TARGET="${IOS_DEPLOYMENT_TARGET:-15.0}"
PLIST="${APP_DIR}/Info.plist"

if [[ ! -f "$MASTER" ]]; then
  echo "error: missing ${MASTER}" >&2
  exit 1
fi
if [[ ! -d "$APP_DIR" || ! -f "$PLIST" ]]; then
  echo "error: app bundle / Info.plist missing under ${APP_DIR}" >&2
  exit 1
fi

if ! python3 -c 'from PIL import Image' 2>/dev/null; then
  pip3 install --user pillow >/dev/null
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/pp-ios-icons.XXXXXX")"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

ASSET_ROOT="${WORK}/Assets.xcassets"
ICON_SET="${ASSET_ROOT}/AppIcon.appiconset"
mkdir -p "$ICON_SET"

cat >"${ASSET_ROOT}/Contents.json" <<'EOF'
{ "info" : { "author" : "pp-browser", "version" : 1 } }
EOF

# Simple filenames — actool warns "invalid extension" on some AppIcon60x60@2x names.
cat >"${ICON_SET}/Contents.json" <<'EOF'
{
  "images" : [
    { "idiom" : "iphone", "size" : "60x60", "scale" : "2x", "filename" : "iphone-60-2x.png" },
    { "idiom" : "iphone", "size" : "60x60", "scale" : "3x", "filename" : "iphone-60-3x.png" },
    { "idiom" : "ipad", "size" : "76x76", "scale" : "2x", "filename" : "ipad-76-2x.png" },
    { "idiom" : "ipad", "size" : "83.5x83.5", "scale" : "2x", "filename" : "ipad-83.5-2x.png" },
    { "idiom" : "ios-marketing", "size" : "1024x1024", "scale" : "1x", "filename" : "ios-marketing-1024.png" }
  ],
  "info" : { "author" : "pp-browser", "version" : 1 }
}
EOF

# Flatten onto white + drop alpha (ASC rejects marketing icons with alpha).
write_rgb_png() {
  local px="$1" out="$2"
  python3 - "$MASTER" "$px" "$out" <<'PY'
from PIL import Image
import sys
master, px, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
im = Image.open(master).convert("RGBA").resize((px, px), Image.Resampling.LANCZOS)
bg = Image.new("RGBA", im.size, (255, 255, 255, 255))
Image.alpha_composite(bg, im).convert("RGB").save(out, format="PNG")
PY
}

write_rgb_png 120 "${ICON_SET}/iphone-60-2x.png"
write_rgb_png 180 "${ICON_SET}/iphone-60-3x.png"
write_rgb_png 152 "${ICON_SET}/ipad-76-2x.png"
write_rgb_png 167 "${ICON_SET}/ipad-83.5-2x.png"
write_rgb_png 1024 "${ICON_SET}/ios-marketing-1024.png"

# Loose PNGs with CFBundleIconFiles naming (ASC still probes these).
write_rgb_png 120 "${APP_DIR}/fiona.g@example.net"
write_rgb_png 180 "${APP_DIR}/carlos.r@example.net"
write_rgb_png 152 "${APP_DIR}/AppIcon76x76@2x~ipad.png"
write_rgb_png 167 "${APP_DIR}/AppIcon83.5x83.5@2x~ipad.png"

PARTIAL="${WORK}/partial.plist"
COMPILED="${WORK}/compiled"
mkdir -p "$COMPILED"

xcrun actool "$ASSET_ROOT" \
  --compile "$COMPILED" \
  --platform iphoneos \
  --minimum-deployment-target "$DEPLOYMENT_TARGET" \
  --app-icon AppIcon \
  --include-all-app-icons \
  --output-partial-info-plist "$PARTIAL" \
  --compress-pngs \
  --notices --warnings \
  >"${WORK}/actool.out" 2>&1 || {
    cat "${WORK}/actool.out" >&2
    exit 1
  }
# Show actool summary without dumping huge XML unless it failed.
if grep -q "Missing Content\|error:" "${WORK}/actool.out"; then
  cat "${WORK}/actool.out" >&2
fi

if [[ ! -f "${COMPILED}/Assets.car" ]]; then
  echo "error: actool did not produce Assets.car" >&2
  cat "${WORK}/actool.out" >&2
  exit 1
fi

cp "${COMPILED}/Assets.car" "${APP_DIR}/Assets.car"
# Do NOT copy actool companion PNGs — they can reintroduce alpha over our RGB flats.

python3 - "$PARTIAL" "$PLIST" <<'PY'
import plistlib, sys, os
partial_path, dest_path = sys.argv[1], sys.argv[2]
partial = {}
if os.path.isfile(partial_path):
    with open(partial_path, "rb") as f:
        partial = plistlib.load(f)
with open(dest_path, "rb") as f:
    dest = plistlib.load(f)
for key in ("CFBundleIcons", "CFBundleIcons~ipad", "CFBundleIconName"):
    if key in partial:
        dest[key] = partial[key]
dest["CFBundleIconName"] = "AppIcon"
iphone = dest.setdefault("CFBundleIcons", {}).setdefault("CFBundlePrimaryIcon", {})
iphone["CFBundleIconName"] = "AppIcon"
iphone["CFBundleIconFiles"] = ["AppIcon60x60"]
ipad = dest.setdefault("CFBundleIcons~ipad", {}).setdefault("CFBundlePrimaryIcon", {})
ipad["CFBundleIconName"] = "AppIcon"
ipad["CFBundleIconFiles"] = ["AppIcon76x76"]
with open(dest_path, "wb") as f:
    plistlib.dump(dest, f, sort_keys=False)
print("merged icon keys ok")
PY

echo "==> Installed Assets.car + RGB icon PNGs into ${APP_DIR}"
