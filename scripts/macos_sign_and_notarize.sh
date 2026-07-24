#!/usr/bin/env bash
# Sign and notarize Frame.app / release DMG for macOS distribution.
#
# Skips gracefully when signing credentials are not configured (unsigned release).
# See packaging/macos/signing.env.example and docs/ops/RELEASE.md.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEFAULT_ENTITLEMENTS="${ROOT}/packaging/macos/Frame.entitlements"

usage() {
  cat <<EOF
Usage: $(basename "$0") <command> [path]

Commands:
  sign-app <Frame.app>       Code-sign the app bundle (hardened runtime)
  notarize <artifact>        Submit .app (zipped), .dmg, or .pkg to Apple notarization
  staple <artifact>          Staple notarization ticket onto artifact
  release <Frame.app> <dmg>  sign-app → notarize dmg → staple (post-cpack convenience)

Environment (certificate — set one of):
  APPLE_CERTIFICATE_BASE64     Base64 .p12 (CI / GitHub Actions)
  APPLE_CERTIFICATE_PATH         Path to .p12 (local)
  APPLE_CERTIFICATE_PASSWORD     .p12 export password

  APPLE_SIGNING_IDENTITY         e.g. "Developer ID Application: Org (TEAMID)"
  APPLE_TEAM_ID                  10-character Team ID (optional verify helper)

Environment (notarization — App Store Connect API key):
  APPLE_NOTARY_KEY_ID
  APPLE_NOTARY_ISSUER_ID
  APPLE_NOTARY_P8_BASE64         Base64 .p8 key (CI)
  APPLE_NOTARY_P8_PATH           Path to AuthKey_*.p8 (local)

Optional:
  MACOS_ENTITLEMENTS             Default: packaging/macos/Frame.entitlements
  MACOS_SKIP_SIGNING=1           Force skip (unsigned)

Local example:
  source packaging/macos/signing.env
  cmake --install build --prefix install
  $(basename "$0") sign-app install/Frame.app
  (cd build && cpack -G DragNDrop)
  $(basename "$0") release install/Frame.app build/pp-browser-0.1.0-macos.dmg
EOF
}

log() {
  printf '==> %s\n' "$*"
}

warn() {
  printf 'warning: %s\n' "$*" >&2
}

require_cmd() {
  local cmd="$1"
  if ! command -v "$cmd" >/dev/null 2>&1; then
    echo "error: required command not found: ${cmd}" >&2
    exit 1
  fi
}

decode_base64_to() {
  local encoded="$1"
  local output="$2"
  if base64 --decode >/dev/null 2>&1 <<<'AA==' ; then
    printf '%s' "$encoded" | base64 --decode >"$output"
  else
    printf '%s' "$encoded" | base64 -D >"$output"
  fi
}

signing_configured() {
  [[ "${MACOS_SKIP_SIGNING:-}" != "1" ]] \
    && { [[ -n "${APPLE_CERTIFICATE_BASE64:-}" ]] || [[ -n "${APPLE_CERTIFICATE_PATH:-}" ]]; } \
    && [[ -n "${APPLE_CERTIFICATE_PASSWORD:-}" ]] \
    && [[ -n "${APPLE_SIGNING_IDENTITY:-}" ]]
}

notary_configured() {
  [[ -n "${APPLE_NOTARY_KEY_ID:-}" ]] \
    && [[ -n "${APPLE_NOTARY_ISSUER_ID:-}" ]] \
    && { [[ -n "${APPLE_NOTARY_P8_BASE64:-}" ]] || [[ -n "${APPLE_NOTARY_P8_PATH:-}" ]]; }
}

ensure_signing_or_skip() {
  if signing_configured; then
    return 0
  fi
  warn "macOS signing credentials not configured; skipping (unsigned artifact)."
  warn "Set APPLE_CERTIFICATE_* and APPLE_SIGNING_IDENTITY, or see packaging/macos/signing.env.example"
  exit 0
}

ensure_notary_or_skip() {
  if notary_configured; then
    return 0
  fi
  warn "Notarization credentials not configured; skipping notarization."
  warn "Set APPLE_NOTARY_KEY_ID, APPLE_NOTARY_ISSUER_ID, and APPLE_NOTARY_P8_*"
  exit 0
}

KEYCHAIN_PATH=""
KEYCHAIN_PASSWORD=""
TEMP_P12=""
TEMP_P8=""

cleanup() {
  if [[ -n "$KEYCHAIN_PATH" && -f "$KEYCHAIN_PATH" ]]; then
    security delete-keychain "$KEYCHAIN_PATH" >/dev/null 2>&1 || true
  fi
  [[ -n "$TEMP_P12" && -f "$TEMP_P12" ]] && rm -f "$TEMP_P12"
  [[ -n "$TEMP_P8" && -f "$TEMP_P8" ]] && rm -f "$TEMP_P8"
}
trap cleanup EXIT

prepare_keychain() {
  require_cmd security
  require_cmd codesign

  local p12_path=""
  if [[ -n "${APPLE_CERTIFICATE_BASE64:-}" ]]; then
    TEMP_P12="$(mktemp "${TMPDIR:-/tmp}/frame-cert.XXXXXX.p12")"
    decode_base64_to "$APPLE_CERTIFICATE_BASE64" "$TEMP_P12"
    p12_path="$TEMP_P12"
  elif [[ -n "${APPLE_CERTIFICATE_PATH:-}" ]]; then
    p12_path="$APPLE_CERTIFICATE_PATH"
  else
    echo "error: APPLE_CERTIFICATE_BASE64 or APPLE_CERTIFICATE_PATH is required" >&2
    exit 1
  fi

  if [[ ! -f "$p12_path" ]]; then
    echo "error: certificate file not found: ${p12_path}" >&2
    exit 1
  fi

  KEYCHAIN_PASSWORD="$(openssl rand -base64 32)"
  KEYCHAIN_PATH="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/frame-signing.keychain-db"

  security create-keychain -p "$KEYCHAIN_PASSWORD" "$KEYCHAIN_PATH"
  security set-keychain-settings -lut 21600 "$KEYCHAIN_PATH"
  security unlock-keychain -p "$KEYCHAIN_PASSWORD" "$KEYCHAIN_PATH"
  security import "$p12_path" \
    -P "$APPLE_CERTIFICATE_PASSWORD" \
    -A -t cert -f pkcs12 \
    -k "$KEYCHAIN_PATH"
  security set-key-partition-list \
    -S apple-tool:,apple:,codesign: \
    -s -k "$KEYCHAIN_PASSWORD" \
    "$KEYCHAIN_PATH"
  security list-keychains -d user -s "$KEYCHAIN_PATH"
}

notary_key_path() {
  if [[ -n "${APPLE_NOTARY_P8_PATH:-}" ]]; then
    if [[ ! -f "$APPLE_NOTARY_P8_PATH" ]]; then
      echo "error: notary key not found: ${APPLE_NOTARY_P8_PATH}" >&2
      exit 1
    fi
    printf '%s' "$APPLE_NOTARY_P8_PATH"
    return
  fi

  TEMP_P8="$(mktemp "${TMPDIR:-/tmp}/AuthKey.XXXXXX.p8")"
  decode_base64_to "$APPLE_NOTARY_P8_BASE64" "$TEMP_P8"
  printf '%s' "$TEMP_P8"
}

cmd_sign_app() {
  local app_path="$1"
  ensure_signing_or_skip

  if [[ ! -d "$app_path" ]]; then
    echo "error: app bundle not found: ${app_path}" >&2
    exit 1
  fi

  local entitlements="${MACOS_ENTITLEMENTS:-$DEFAULT_ENTITLEMENTS}"
  if [[ ! -f "$entitlements" ]]; then
    echo "error: entitlements file not found: ${entitlements}" >&2
    exit 1
  fi

  prepare_keychain

  log "Signing ${app_path}"
  log "Identity: ${APPLE_SIGNING_IDENTITY}"

  # Sign nested Mach-O binaries first (no entitlements), then the bundle.
  while IFS= read -r -d '' binary; do
    if file "$binary" | grep -q 'Mach-O'; then
      codesign --force --options runtime \
        --sign "$APPLE_SIGNING_IDENTITY" \
        --timestamp \
        "$binary"
    fi
  done < <(find "$app_path" -type f -print0)

  codesign --force --options runtime \
    --sign "$APPLE_SIGNING_IDENTITY" \
    --entitlements "$entitlements" \
    --timestamp \
    "$app_path"

  codesign --verify --deep --strict --verbose=2 "$app_path"
  log "Signed ${app_path}"
}

submission_path_for() {
  local artifact="$1"
  case "$artifact" in
    *.app)
      local zip_path="${artifact%/}.zip"
      log "Zipping ${artifact} for notarization → ${zip_path}"
      ditto -c -k --keepParent "$artifact" "$zip_path"
      printf '%s' "$zip_path"
      ;;
    *)
      printf '%s' "$artifact"
      ;;
  esac
}

cmd_notarize() {
  local artifact="$1"
  ensure_notary_or_skip

  if [[ ! -e "$artifact" ]]; then
    echo "error: artifact not found: ${artifact}" >&2
    exit 1
  fi

  require_cmd xcrun

  local submit_path
  submit_path="$(submission_path_for "$artifact")"
  local p8
  p8="$(notary_key_path)"

  log "Submitting ${submit_path} for notarization"
  xcrun notarytool submit "$submit_path" \
    --key "$p8" \
    --key-id "$APPLE_NOTARY_KEY_ID" \
    --issuer "$APPLE_NOTARY_ISSUER_ID" \
    --wait

  if [[ "$submit_path" != "$artifact" && -f "$submit_path" ]]; then
    rm -f "$submit_path"
  fi

  log "Notarization accepted for ${artifact}"
}

cmd_staple() {
  local artifact="$1"
  ensure_notary_or_skip

  if [[ ! -e "$artifact" ]]; then
    echo "error: artifact not found: ${artifact}" >&2
    exit 1
  fi

  require_cmd xcrun
  log "Stapling ${artifact}"
  xcrun stapler staple "$artifact"
  xcrun stapler validate "$artifact" || true
  spctl -a -t exec -vv "$artifact" 2>/dev/null || spctl -a -t open -vv "$artifact" || true
  log "Stapled ${artifact}"
}

cmd_release() {
  local app_path="$1"
  local dmg_path="$2"

  cmd_sign_app "$app_path"

  if notary_configured; then
    cmd_notarize "$dmg_path"
    cmd_staple "$dmg_path"
  else
    ensure_notary_or_skip
  fi
}

main() {
  if [[ $# -lt 1 ]]; then
    usage
    exit 1
  fi

  local command="$1"
  shift

  case "$command" in
    sign-app)
      [[ $# -eq 1 ]] || { usage; exit 1; }
      cmd_sign_app "$1"
      ;;
    notarize)
      [[ $# -eq 1 ]] || { usage; exit 1; }
      cmd_notarize "$1"
      ;;
    staple)
      [[ $# -eq 1 ]] || { usage; exit 1; }
      cmd_staple "$1"
      ;;
    release)
      [[ $# -eq 2 ]] || { usage; exit 1; }
      cmd_release "$1" "$2"
      ;;
    -h|--help|help)
      usage
      ;;
    *)
      echo "error: unknown command: ${command}" >&2
      usage
      exit 1
      ;;
  esac
}

main "$@"
