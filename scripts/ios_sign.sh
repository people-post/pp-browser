#!/usr/bin/env bash
# Sign PP.app for iOS device distribution (development / ad-hoc / TestFlight / App Store).
#
# Skips gracefully when signing credentials are not configured.
# See packaging/ios/signing.env.example and docs/ops/IOS_BUILD.md.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Sync with pbr::kProductBundleName / kProductSlug in src/base/runtime/ProductBranding.h
PRODUCT_BUNDLE_NAME="${PP_BROWSER_PRODUCT_BUNDLE_NAME:-PP}"
PRODUCT_SLUG="${PP_BROWSER_PRODUCT_SLUG:-pp-browser}"
DEFAULT_ENTITLEMENTS="${ROOT}/packaging/ios/${PRODUCT_SLUG}.entitlements"

usage() {
  cat <<EOF
Usage: $(basename "$0") <command> [path]

Commands:
  sign-app <PP.app>     Code-sign an iOS .app (development or distribution)
  export-ipa <PP.app>   Sign + produce dist-ios/*.ipa for TestFlight / install
  upload-ipa <file.ipa> Upload IPA to App Store Connect (altool + ASC API key)
  verify <PP.app>       Verify codesign on an iOS app bundle

Environment (local — source packaging/ios/signing.env):
  IOS_BUNDLE_IDENTIFIER          dev.pp-browser.ios
  IOS_DEVELOPMENT_TEAM           YOUR_TEAM_ID

  Development (device USB):
  IOS_SIGNING_IDENTITY           e.g. "Apple Development: Name (TEAMID)"
  IOS_PROVISIONING_PROFILE_PATH  Path to Development .mobileprovision

  Distribution (TestFlight / App Store):
  IOS_EXPORT_METHOD              development | ad-hoc | app-store | enterprise
  IOS_DISTRIBUTION_SIGNING_IDENTITY
                                 e.g. "Apple Distribution: Org (TEAMID)"
  IOS_DISTRIBUTION_PROVISIONING_PROFILE_PATH
                                 Path to App Store / Ad Hoc .mobileprovision

  Upload (optional — App Store Connect API key):
  IOS_ASC_KEY_ID                 AuthKey_XXXXXX.p8 key id
  IOS_ASC_ISSUER_ID              Issuer UUID from App Store Connect → Users and Access → Keys
  IOS_ASC_P8_PATH                Path to AuthKey_XXXXXX.p8
  IOS_ASC_P8_BASE64              Or base64 of the .p8 (CI)

Environment (CI — optional):
  IOS_CERTIFICATE_BASE64
  IOS_CERTIFICATE_PASSWORD
  IOS_PROVISIONING_PROFILE_BASE64

Optional:
  IOS_ENTITLEMENTS               Default: packaging/ios/pp-browser.entitlements
  IOS_EXPORT_OPTIONS_PLIST       Default: packaging/ios/ExportOptions.plist
  PP_BROWSER_VERSION             Marketing version → CFBundleShortVersionString
  PP_BROWSER_BUILD_NUMBER        Build number → CFBundleVersion (must bump each upload)
  IOS_SKIP_SIGNING=1             Force skip

Local development:
  source packaging/ios/signing.env
  ./scripts/ios_build.sh device
  ./scripts/ios_sign.sh sign-app install-ios/PP.app

TestFlight IPA:
  source packaging/ios/signing.env   # with IOS_EXPORT_METHOD=app-store + Distribution vars
  CMAKE_BUILD_TYPE=Release ./scripts/ios_build.sh device
  ./scripts/ios_sign.sh export-ipa install-ios/PP.app
  ./scripts/ios_sign.sh upload-ipa dist-ios/pp-browser.ipa
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

# Resolve export method: development | ad-hoc | app-store | enterprise
export_method() {
  printf '%s' "${IOS_EXPORT_METHOD:-development}"
}

is_distribution_method() {
  case "$(export_method)" in
    app-store|ad-hoc|enterprise) return 0 ;;
    *) return 1 ;;
  esac
}

resolved_signing_identity() {
  if is_distribution_method && [[ -n "${IOS_DISTRIBUTION_SIGNING_IDENTITY:-}" ]]; then
    printf '%s' "$IOS_DISTRIBUTION_SIGNING_IDENTITY"
  else
    printf '%s' "${IOS_SIGNING_IDENTITY:-}"
  fi
}

resolved_profile_path() {
  if is_distribution_method && [[ -n "${IOS_DISTRIBUTION_PROVISIONING_PROFILE_PATH:-}" ]]; then
    printf '%s' "$IOS_DISTRIBUTION_PROVISIONING_PROFILE_PATH"
  else
    printf '%s' "${IOS_PROVISIONING_PROFILE_PATH:-}"
  fi
}

# Secure timestamp for App Store / Ad Hoc; none for local USB development.
codesign_timestamp_flag() {
  if is_distribution_method; then
    printf '%s' "--timestamp"
  else
    printf '%s' "--timestamp=none"
  fi
}

signing_configured() {
  local identity profile
  identity="$(resolved_signing_identity)"
  profile="$(resolved_profile_path)"
  [[ "${IOS_SKIP_SIGNING:-}" != "1" ]] \
    && [[ -n "$identity" ]] \
    && [[ -n "${IOS_DEVELOPMENT_TEAM:-}" ]] \
    && { [[ -n "$profile" ]] || [[ -n "${IOS_PROVISIONING_PROFILE_BASE64:-}" ]]; }
}

ensure_signing_or_skip() {
  if signing_configured; then
    return 0
  fi
  warn "iOS signing credentials not configured; skipping."
  warn "Copy packaging/ios/signing.env.example → signing.env and fill placeholders."
  if is_distribution_method; then
    warn "For TestFlight set IOS_EXPORT_METHOD=app-store plus IOS_DISTRIBUTION_* vars."
  fi
  exit 0
}

TEMP_P12=""
TEMP_PROFILE=""
TEMP_ASC_P8=""
KEYCHAIN_PATH=""
KEYCHAIN_PASSWORD=""

cleanup() {
  # Use if/fi (not `[[ … ]] && cmd`) so an empty path does not make the EXIT
  # trap return non-zero under `set -e` and abort callers like ios_build.sh.
  if [[ -n "${KEYCHAIN_PATH:-}" && -f "$KEYCHAIN_PATH" ]]; then
    security delete-keychain "$KEYCHAIN_PATH" >/dev/null 2>&1 || true
  fi
  if [[ -n "${TEMP_P12:-}" && -f "$TEMP_P12" ]]; then
    rm -f "$TEMP_P12"
  fi
  if [[ -n "${TEMP_PROFILE:-}" && -f "$TEMP_PROFILE" ]]; then
    rm -f "$TEMP_PROFILE"
  fi
  if [[ -n "${TEMP_ASC_P8:-}" && -f "$TEMP_ASC_P8" ]]; then
    rm -f "$TEMP_ASC_P8"
  fi
  return 0
}
trap cleanup EXIT

prepare_profile() {
  local profile_dir="${HOME}/Library/MobileDevice/Provisioning Profiles"
  mkdir -p "$profile_dir"

  local profile_path
  profile_path="$(resolved_profile_path)"
  if [[ -n "$profile_path" ]]; then
    if [[ ! -f "$profile_path" ]]; then
      echo "error: provisioning profile not found: ${profile_path}" >&2
      exit 1
    fi
    cp "$profile_path" "$profile_dir/"
    return
  fi

  if [[ -n "${IOS_PROVISIONING_PROFILE_BASE64:-}" ]]; then
    TEMP_PROFILE="$(mktemp "${TMPDIR:-/tmp}/frame-ios.XXXXXX.mobileprovision")"
    decode_base64_to "$IOS_PROVISIONING_PROFILE_BASE64" "$TEMP_PROFILE"
    cp "$TEMP_PROFILE" "$profile_dir/"
    return
  fi

  echo "error: provisioning profile path or IOS_PROVISIONING_PROFILE_BASE64 required" >&2
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

# Stamp marketing / build versions into the app Info.plist before codesign.
stamp_bundle_versions() {
  local app_path="$1"
  local plist="${app_path}/Info.plist"
  [[ -f "$plist" ]] || return 0

  local short_ver build_ver
  short_ver="${PP_BROWSER_VERSION:-}"
  build_ver="${PP_BROWSER_BUILD_NUMBER:-${PP_BROWSER_RELEASE_VERSION:-}}"

  if [[ -n "$short_ver" ]]; then
    /usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString ${short_ver}" "$plist" 2>/dev/null \
      || plutil -replace CFBundleShortVersionString -string "$short_ver" "$plist"
    log "CFBundleShortVersionString=${short_ver}"
  fi
  if [[ -n "$build_ver" ]]; then
    /usr/libexec/PlistBuddy -c "Set :CFBundleVersion ${build_ver}" "$plist" 2>/dev/null \
      || plutil -replace CFBundleVersion -string "$build_ver" "$plist"
    log "CFBundleVersion=${build_ver}"
  elif is_distribution_method; then
    warn "PP_BROWSER_BUILD_NUMBER unset — App Store Connect rejects reused CFBundleVersion"
  fi
}

# Extract entitlements from the active provisioning profile; strip get-task-allow for distribution.
extract_profile_entitlements() {
  local profile_src="$1"
  local out_file="$2"

  local profile_plist
  profile_plist="$(mktemp "${TMPDIR:-/tmp}/frame-profile.XXXXXX.plist")"
  if ! security cms -D -i "$profile_src" >"$profile_plist" 2>/dev/null \
      || ! plutil -extract Entitlements xml1 -o "$out_file" "$profile_plist" 2>/dev/null \
      || [[ ! -s "$out_file" ]]; then
    rm -f "$profile_plist" "$out_file"
    return 1
  fi
  rm -f "$profile_plist"

  if is_distribution_method; then
    # App Store / Ad Hoc profiles must not allow debugger attach.
    /usr/libexec/PlistBuddy -c "Delete :get-task-allow" "$out_file" 2>/dev/null || true
    plutil -remove get-task-allow "$out_file" 2>/dev/null || true
  fi
  return 0
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

  local identity
  identity="$(resolved_signing_identity)"
  local method
  method="$(export_method)"
  local ts_flag
  ts_flag="$(codesign_timestamp_flag)"

  local entitlements="${IOS_ENTITLEMENTS:-$DEFAULT_ENTITLEMENTS}"
  local entitlements_tmp=""
  local profile_src=""
  local resolved_profile
  resolved_profile="$(resolved_profile_path)"
  if [[ -n "$resolved_profile" && -f "$resolved_profile" ]]; then
    profile_src="$resolved_profile"
  elif [[ -n "${TEMP_PROFILE:-}" && -f "${TEMP_PROFILE}" ]]; then
    profile_src="${TEMP_PROFILE}"
  fi

  if [[ -n "$profile_src" ]]; then
    entitlements_tmp="$(mktemp "${TMPDIR:-/tmp}/frame-ents.XXXXXX.plist")"
    if extract_profile_entitlements "$profile_src" "$entitlements_tmp"; then
      entitlements="$entitlements_tmp"
      log "Using entitlements from provisioning profile ($(basename "$profile_src"))"
    else
      rm -f "$entitlements_tmp"
      entitlements_tmp=""
      if ! is_distribution_method \
          && [[ -n "${IOS_DEVELOPMENT_TEAM:-}" && -n "${IOS_BUNDLE_IDENTIFIER:-}" ]]; then
        entitlements_tmp="$(mktemp "${TMPDIR:-/tmp}/frame-ents.XXXXXX.plist")"
        cat >"$entitlements_tmp" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>application-identifier</key>
  <string>${IOS_DEVELOPMENT_TEAM}.${IOS_BUNDLE_IDENTIFIER}</string>
  <key>com.apple.developer.team-identifier</key>
  <string>${IOS_DEVELOPMENT_TEAM}</string>
  <key>get-task-allow</key>
  <true/>
  <key>keychain-access-groups</key>
  <array>
    <string>${IOS_DEVELOPMENT_TEAM}.*</string>
  </array>
</dict>
</plist>
EOF
        entitlements="$entitlements_tmp"
        warn "Synthesized development entitlements for ${IOS_DEVELOPMENT_TEAM}.${IOS_BUNDLE_IDENTIFIER}"
      else
        warn "Could not extract entitlements from profile; falling back to ${entitlements}"
      fi
    fi
  fi

  if [[ ! -f "$entitlements" ]]; then
    echo "error: entitlements not found: ${entitlements}" >&2
    exit 1
  fi

  if is_distribution_method && [[ -z "$profile_src" ]]; then
    echo "error: distribution signing requires an App Store / Ad Hoc provisioning profile" >&2
    exit 1
  fi

  local bundle_id="${IOS_BUNDLE_IDENTIFIER:-dev.pp-browser.ios}"
  log "Signing ${app_path}"
  log "Method: ${method}"
  log "Identity: ${identity}"
  log "Team: ${IOS_DEVELOPMENT_TEAM}"
  log "Bundle ID: ${bundle_id}"
  log "Timestamp: ${ts_flag}"

  if [[ -z "$profile_src" ]]; then
    echo "error: no provisioning profile to embed" >&2
    exit 1
  fi
  cp "$profile_src" "${app_path}/embedded.mobileprovision"
  log "Embedded $(basename "$profile_src")"

  if [[ -f "${app_path}/Info.plist" ]]; then
    /usr/libexec/PlistBuddy -c "Set :CFBundleIdentifier ${bundle_id}" "${app_path}/Info.plist" 2>/dev/null \
      || plutil -replace CFBundleIdentifier -string "$bundle_id" "${app_path}/Info.plist"
  fi
  stamp_bundle_versions "$app_path"

  # Drop Finder/zip junk that can break device / ASC verification.
  rm -rf "${app_path}/META-INF" "${app_path}/.DS_Store"
  find "$app_path" -name '.DS_Store' -delete 2>/dev/null || true

  # Sign nested Mach-O first, then the bundle (deepest-first via find).
  while IFS= read -r -d '' binary; do
    if file "$binary" | grep -q 'Mach-O'; then
      # shellcheck disable=SC2086
      codesign --force --sign "$identity" ${ts_flag} "$binary" || true
    fi
  done < <(find "$app_path" -type f ! -name 'embedded.mobileprovision' -print0)

  # shellcheck disable=SC2086
  codesign --force --sign "$identity" \
    --entitlements "$entitlements" \
    --generate-entitlement-der \
    ${ts_flag} \
    "$app_path"

  [[ -n "$entitlements_tmp" ]] && rm -f "$entitlements_tmp"

  codesign --verify --deep --strict --verbose=2 "$app_path"
  log "Signed ${app_path} (${method})"
}

cmd_verify() {
  local app_path="$1"
  require_cmd codesign
  codesign --verify --deep --strict --verbose=2 "$app_path"
  if command -v codesign >/dev/null 2>&1; then
    log "Entitlements:"
    codesign -d --entitlements :- "$app_path" 2>/dev/null || true
  fi
}

cmd_export_ipa() {
  local app_path="$1"
  ensure_signing_or_skip
  require_cmd ditto

  local export_plist="${IOS_EXPORT_OPTIONS_PLIST:-${ROOT}/packaging/ios/ExportOptions.plist}"
  if [[ -f "$export_plist" ]]; then
    local plist_method=""
    plist_method="$(/usr/libexec/PlistBuddy -c 'Print :method' "$export_plist" 2>/dev/null || true)"
    if [[ -n "$plist_method" && -z "${IOS_EXPORT_METHOD:-}" ]]; then
      IOS_EXPORT_METHOD="$plist_method"
      log "IOS_EXPORT_METHOD from ExportOptions.plist: ${IOS_EXPORT_METHOD}"
    fi
  elif is_distribution_method; then
    warn "ExportOptions.plist not found (optional for zip IPA): ${export_plist}"
    warn "cp packaging/ios/ExportOptions.plist.example packaging/ios/ExportOptions.plist"
  fi

  if ! is_distribution_method; then
    warn "IOS_EXPORT_METHOD=$(export_method) — set to app-store for TestFlight"
  fi

  cmd_sign_app "$app_path"

  local payload_dir
  payload_dir="$(mktemp -d "${TMPDIR:-/tmp}/${PRODUCT_SLUG}-ipa.XXXXXX")"
  mkdir -p "${payload_dir}/Payload"
  ditto "$app_path" "${payload_dir}/Payload/$(basename "$app_path")"

  local short_ver build_ver ipa_name
  short_ver="${PP_BROWSER_VERSION:-}"
  build_ver="${PP_BROWSER_BUILD_NUMBER:-${PP_BROWSER_RELEASE_VERSION:-}}"
  if [[ -n "$short_ver" && -n "$build_ver" ]]; then
    ipa_name="${PRODUCT_SLUG}-${short_ver}-${build_ver}.ipa"
  elif [[ -n "$short_ver" ]]; then
    ipa_name="${PRODUCT_SLUG}-${short_ver}.ipa"
  else
    ipa_name="${PRODUCT_SLUG}.ipa"
  fi

  log "Creating IPA from ${app_path}"
  (cd "$payload_dir" && zip -qr "${ipa_name}" Payload)
  mkdir -p "${ROOT}/dist-ios"
  mv "${payload_dir}/${ipa_name}" "${ROOT}/dist-ios/${ipa_name}"
  # Stable symlink for upload scripts / docs.
  ln -sfn "${ipa_name}" "${ROOT}/dist-ios/${PRODUCT_SLUG}.ipa"
  log "Wrote ${ROOT}/dist-ios/${ipa_name}"
  log "Also: ${ROOT}/dist-ios/${PRODUCT_SLUG}.ipa → ${ipa_name}"
  rm -rf "$payload_dir"
}

asc_configured() {
  [[ -n "${IOS_ASC_KEY_ID:-}" ]] \
    && [[ -n "${IOS_ASC_ISSUER_ID:-}" ]] \
    && { [[ -n "${IOS_ASC_P8_PATH:-}" ]] || [[ -n "${IOS_ASC_P8_BASE64:-}" ]]; }
}

prepare_asc_key() {
  local key_dir="${HOME}/.appstoreconnect/private_keys"
  mkdir -p "$key_dir"
  local dest="${key_dir}/AuthKey_${IOS_ASC_KEY_ID}.p8"

  if [[ -n "${IOS_ASC_P8_PATH:-}" ]]; then
    if [[ ! -f "$IOS_ASC_P8_PATH" ]]; then
      echo "error: ASC .p8 not found: ${IOS_ASC_P8_PATH}" >&2
      exit 1
    fi
    cp "$IOS_ASC_P8_PATH" "$dest"
    return
  fi

  if [[ -n "${IOS_ASC_P8_BASE64:-}" ]]; then
    TEMP_ASC_P8="$(mktemp "${TMPDIR:-/tmp}/frame-asc.XXXXXX.p8")"
    decode_base64_to "$IOS_ASC_P8_BASE64" "$TEMP_ASC_P8"
    cp "$TEMP_ASC_P8" "$dest"
    return
  fi

  echo "error: IOS_ASC_P8_PATH or IOS_ASC_P8_BASE64 required" >&2
  exit 1
}

cmd_upload_ipa() {
  local ipa_path="$1"
  if [[ ! -f "$ipa_path" ]]; then
    echo "error: IPA not found: ${ipa_path}" >&2
    exit 1
  fi

  if ! asc_configured; then
    echo "error: App Store Connect API key not configured" >&2
    echo "hint: set IOS_ASC_KEY_ID, IOS_ASC_ISSUER_ID, and IOS_ASC_P8_PATH in signing.env" >&2
    echo "hint: or upload ${ipa_path} with the Transporter app" >&2
    exit 1
  fi

  require_cmd xcrun
  prepare_asc_key

  log "Uploading ${ipa_path} to App Store Connect"
  log "ASC key: ${IOS_ASC_KEY_ID}"
  # altool resolves AuthKey_<id>.p8 under ~/.appstoreconnect/private_keys
  xcrun altool --upload-app \
    --type ios \
    --file "$ipa_path" \
    --apiKey "$IOS_ASC_KEY_ID" \
    --apiIssuer "$IOS_ASC_ISSUER_ID"
  log "Upload submitted — wait for processing in App Store Connect, then answer export compliance"
}

main() {
  [[ $# -ge 1 ]] || { usage; exit 1; }
  local command="$1"
  shift
  case "$command" in
    sign-app) [[ $# -eq 1 ]] || { usage; exit 1; }; cmd_sign_app "$1" ;;
    verify)   [[ $# -eq 1 ]] || { usage; exit 1; }; cmd_verify "$1" ;;
    export-ipa) [[ $# -eq 1 ]] || { usage; exit 1; }; cmd_export_ipa "$1" ;;
    upload-ipa) [[ $# -eq 1 ]] || { usage; exit 1; }; cmd_upload_ipa "$1" ;;
    -h|--help|help) usage ;;
    *) echo "error: unknown command: ${command}" >&2; usage; exit 1 ;;
  esac
}

main "$@"
