#!/usr/bin/env bash
# Sign Frame.app for iOS device distribution (development / ad-hoc / TestFlight prep).
#
# Skips gracefully when signing credentials are not configured.
# See packaging/ios/signing.env.example and docs/ops/IOS_BUILD.md.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEFAULT_ENTITLEMENTS="${ROOT}/packaging/ios/Frame.entitlements"

usage() {
  cat <<EOF
Usage: $(basename "$0") <command> [path]

Commands:
  sign-app <Frame.app>     Code-sign an iOS .app for device install
  export-ipa <Frame.app>   Produce a signed .ipa (requires archive or app + ExportOptions)
  verify <Frame.app>       Verify codesign on an iOS app bundle

Environment (local — source packaging/ios/signing.env):
  IOS_BUNDLE_IDENTIFIER          dev.frame.ios
  IOS_DEVELOPMENT_TEAM           YOUR_TEAM_ID
  IOS_SIGNING_IDENTITY           e.g. "Apple Development: Name (TEAMID)"
  IOS_PROVISIONING_PROFILE_PATH  Path to .mobileprovision

Environment (CI — optional):
  IOS_CERTIFICATE_BASE64
  IOS_CERTIFICATE_PASSWORD
  IOS_PROVISIONING_PROFILE_BASE64

Optional:
  IOS_ENTITLEMENTS               Default: packaging/ios/Frame.entitlements
  IOS_EXPORT_METHOD              development | ad-hoc | app-store | enterprise
  IOS_EXPORT_OPTIONS_PLIST       Default: packaging/ios/ExportOptions.plist
  IOS_SKIP_SIGNING=1             Force skip

Local example:
  source packaging/ios/signing.env
  ./scripts/ios_build.sh device install
  ./scripts/ios_sign.sh sign-app install-ios/Frame.app
EOF
}

log() { printf '==> %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }

require_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "error: required command not found: $1" >&2
    exit 1
  fi
}

decode_base64_to() {
  local encoded="$1" output="$2"
  if base64 --decode >/dev/null 2>&1 <<<'AA=='; then
    printf '%s' "$encoded" | base64 --decode >"$output"
  else
    printf '%s' "$encoded" | base64 -D >"$output"
  fi
}

signing_configured() {
  [[ "${IOS_SKIP_SIGNING:-}" != "1" ]] \
    && [[ -n "${IOS_SIGNING_IDENTITY:-}" ]] \
    && [[ -n "${IOS_DEVELOPMENT_TEAM:-}" ]] \
    && { [[ -n "${IOS_PROVISIONING_PROFILE_PATH:-}" ]] || [[ -n "${IOS_PROVISIONING_PROFILE_BASE64:-}" ]]; }
}

ensure_signing_or_skip() {
  if signing_configured; then
    return 0
  fi
  warn "iOS signing credentials not configured; skipping."
  warn "Copy packaging/ios/signing.env.example → signing.env and fill placeholders."
  exit 0
}

TEMP_P12=""
TEMP_PROFILE=""
KEYCHAIN_PATH=""
KEYCHAIN_PASSWORD=""

cleanup() {
  [[ -n "$KEYCHAIN_PATH" && -f "$KEYCHAIN_PATH" ]] && security delete-keychain "$KEYCHAIN_PATH" >/dev/null 2>&1 || true
  [[ -n "$TEMP_P12" && -f "$TEMP_P12" ]] && rm -f "$TEMP_P12"
  [[ -n "$TEMP_PROFILE" && -f "$TEMP_PROFILE" ]] && rm -f "$TEMP_PROFILE"
}
trap cleanup EXIT

prepare_profile() {
  local profile_dir="${HOME}/Library/MobileDevice/Provisioning Profiles"
  mkdir -p "$profile_dir"

  if [[ -n "${IOS_PROVISIONING_PROFILE_PATH:-}" ]]; then
    if [[ ! -f "$IOS_PROVISIONING_PROFILE_PATH" ]]; then
      echo "error: provisioning profile not found: ${IOS_PROVISIONING_PROFILE_PATH}" >&2
      exit 1
    fi
    cp "$IOS_PROVISIONING_PROFILE_PATH" "$profile_dir/"
    return
  fi

  if [[ -n "${IOS_PROVISIONING_PROFILE_BASE64:-}" ]]; then
    TEMP_PROFILE="$(mktemp "${TMPDIR:-/tmp}/frame-ios.XXXXXX.mobileprovision")"
    decode_base64_to "$IOS_PROVISIONING_PROFILE_BASE64" "$TEMP_PROFILE"
    cp "$TEMP_PROFILE" "$profile_dir/"
    return
  fi

  echo "error: IOS_PROVISIONING_PROFILE_PATH or IOS_PROVISIONING_PROFILE_BASE64 required" >&2
  exit 1
}

prepare_keychain() {
  if [[ -z "${IOS_CERTIFICATE_BASE64:-}" && -z "${IOS_CERTIFICATE_PATH:-}" ]]; then
    return 0
  fi

  require_cmd security
  local p12_path=""
  if [[ -n "${IOS_CERTIFICATE_BASE64:-}" ]]; then
    TEMP_P12="$(mktemp "${TMPDIR:-/tmp}/frame-ios-cert.XXXXXX.p12")"
    decode_base64_to "$IOS_CERTIFICATE_BASE64" "$TEMP_P12"
    p12_path="$TEMP_P12"
  else
    p12_path="$IOS_CERTIFICATE_PATH"
  fi

  KEYCHAIN_PASSWORD="$(openssl rand -base64 32)"
  KEYCHAIN_PATH="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/frame-ios-signing.keychain-db"
  security create-keychain -p "$KEYCHAIN_PASSWORD" "$KEYCHAIN_PATH"
  security set-keychain-settings -lut 21600 "$KEYCHAIN_PATH"
  security unlock-keychain -p "$KEYCHAIN_PASSWORD" "$KEYCHAIN_PATH"
  security import "$p12_path" \
    -P "${IOS_CERTIFICATE_PASSWORD:?IOS_CERTIFICATE_PASSWORD required with certificate}" \
    -A -t cert -f pkcs12 -k "$KEYCHAIN_PATH"
  security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$KEYCHAIN_PASSWORD" "$KEYCHAIN_PATH"
  security list-keychains -d user -s "$KEYCHAIN_PATH"
}

cmd_sign_app() {
  local app_path="$1"
  ensure_signing_or_skip

  if [[ ! -d "$app_path" ]]; then
    echo "error: app bundle not found: ${app_path}" >&2
    exit 1
  fi

  require_cmd codesign
  prepare_keychain
  prepare_profile

  local entitlements="${IOS_ENTITLEMENTS:-$DEFAULT_ENTITLEMENTS}"
  if [[ ! -f "$entitlements" ]]; then
    echo "error: entitlements not found: ${entitlements}" >&2
    exit 1
  fi

  local bundle_id="${IOS_BUNDLE_IDENTIFIER:-dev.frame.ios}"
  log "Signing ${app_path}"
  log "Identity: ${IOS_SIGNING_IDENTITY}"
  log "Team: ${IOS_DEVELOPMENT_TEAM}"
  log "Bundle ID: ${bundle_id}"

  while IFS= read -r -d '' binary; do
    if file "$binary" | grep -q 'Mach-O'; then
      codesign --force --sign "$IOS_SIGNING_IDENTITY" --timestamp "$binary" || true
    fi
  done < <(find "$app_path" -type f -print0)

  codesign --force --sign "$IOS_SIGNING_IDENTITY" \
    --entitlements "$entitlements" \
    --timestamp \
    "$app_path"

  codesign --verify --deep --strict --verbose=2 "$app_path"
  log "Signed ${app_path}"
}

cmd_verify() {
  local app_path="$1"
  require_cmd codesign
  codesign --verify --deep --strict --verbose=2 "$app_path"
}

cmd_export_ipa() {
  local app_path="$1"
  ensure_signing_or_skip
  require_cmd xcrun

  local export_plist="${IOS_EXPORT_OPTIONS_PLIST:-${ROOT}/packaging/ios/ExportOptions.plist}"
  if [[ ! -f "$export_plist" ]]; then
    echo "error: export options not found: ${export_plist}" >&2
    echo "hint: cp packaging/ios/ExportOptions.plist.example packaging/ios/ExportOptions.plist" >&2
    exit 1
  fi

  cmd_sign_app "$app_path"

  local payload_dir="$(mktemp -d "${TMPDIR:-/tmp}/frame-ipa.XXXXXX")"
  local ipa_dir="${payload_dir}/export"
  mkdir -p "${payload_dir}/Payload"
  ditto "$app_path" "${payload_dir}/Payload/$(basename "$app_path")"

  log "Creating IPA from ${app_path}"
  (cd "$payload_dir" && zip -qr Frame.ipa Payload)
  mkdir -p "${ROOT}/dist-ios"
  mv "${payload_dir}/Frame.ipa" "${ROOT}/dist-ios/Frame.ipa"
  log "Wrote ${ROOT}/dist-ios/Frame.ipa"
  rm -rf "$payload_dir"
}

main() {
  [[ $# -ge 1 ]] || { usage; exit 1; }
  local command="$1"
  shift
  case "$command" in
    sign-app) [[ $# -eq 1 ]] || { usage; exit 1; }; cmd_sign_app "$1" ;;
    verify)   [[ $# -eq 1 ]] || { usage; exit 1; }; cmd_verify "$1" ;;
    export-ipa) [[ $# -eq 1 ]] || { usage; exit 1; }; cmd_export_ipa "$1" ;;
    -h|--help|help) usage ;;
    *) echo "error: unknown command: ${command}" >&2; usage; exit 1 ;;
  esac
}

main "$@"
