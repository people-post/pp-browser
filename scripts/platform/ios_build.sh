#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# Sync with pbr::kProductBundleName in src/foundation/runtime/ProductBranding.h
PRODUCT_BUNDLE_NAME="${PP_BROWSER_PRODUCT_BUNDLE_NAME:-PP}"

# Remember whether the caller set IOS_PLATFORM before our default (used by resolve_ios_platform).
if [[ -n "${IOS_PLATFORM+x}" ]]; then
  IOS_PLATFORM_EXPLICIT=1
fi
IOS_PLATFORM="${IOS_PLATFORM:-simulator}"
BUILD_TYPE="${CMAKE_BUILD_TYPE:-Debug}"
INSTALL_PREFIX="${INSTALL_PREFIX:-${ROOT}/install-ios}"
GENERATOR="${IOS_CMAKE_GENERATOR:-Ninja}"

usage() {
  cat <<EOF
Usage: $(basename "$0") <command>

Commands:
  configure-sim     Configure CMake for iOS Simulator (arm64)
  configure-device  Configure CMake for iOS device (arm64)
  build             Build the configured tree
  install           cmake --install into INSTALL_PREFIX (default: install-ios/)
  sim               configure-sim + build + install (simulator .app)
  device            configure-device + build + install (device .app)
  run-sim           Boot a simulator if needed, install PP.app, and launch
  run-device         Sign (if configured) and install+launch on a connected iPhone
  ipa               Release device build + install + export TestFlight IPA
  upload-ipa        Upload dist-ios/pp-browser.ipa (or path) to App Store Connect
  xcode             Configure with -G Xcode (open in Xcode for debugging)
  clean             Remove build-ios-* directories

Environment:
  IOS_PLATFORM              simulator (default) or device
  CMAKE_BUILD_TYPE          Debug (default) or Release
  INSTALL_PREFIX            Output prefix for cmake --install
  PP_BROWSER_VERSION        Marketing version (e.g. 0.1.0) → CFBundleShortVersionString
  PP_BROWSER_BUILD_NUMBER   Build number for App Store Connect (must bump each upload)
  PP_BROWSER_RELEASE_VERSION  Fallback build string if BUILD_NUMBER unset
  IOS_CMAKE_GENERATOR       Ninja (default) or Xcode
  IOS_SIMULATOR_UDID        Target a specific simulator (optional; otherwise newest iPhone)
  IOS_DEVICE_UDID           Target a specific physical device (optional; otherwise first paired iPhone)
  IOS_DEPLOYMENT_TARGET     Minimum iOS version (default: 15.0; sets CMAKE_OSX_DEPLOYMENT_TARGET)

Requires (macOS only):
  Xcode matching the device iOS major (iOS 26.x → Xcode 26.x)
  CMake 3.24+
  Ninja (recommended)

Signing (device / TestFlight): see packaging/ios/signing.env.example and scripts/platform/ios_sign.sh
Docs: docs/ops/IOS_BUILD.md
EOF
}

require_macos() {
  if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "error: iOS builds require macOS with Xcode" >&2
    exit 1
  fi
  if ! xcode-select -p >/dev/null 2>&1; then
    echo "error: Xcode command-line tools not found (xcode-select --install)" >&2
    exit 1
  fi
}

build_dir() {
  if [[ "${IOS_PLATFORM}" == "device" ]]; then
    printf '%s' "${ROOT}/build-ios-device"
  else
    printf '%s' "${ROOT}/build-ios-simulator"
  fi
}

# When IOS_PLATFORM was left at the script default, prefer the more recently
# configured tree so `build`/`install` after `configure-device` hit device.
resolve_ios_platform() {
  if [[ -n "${IOS_PLATFORM_EXPLICIT:-}" ]]; then
    return 0
  fi
  local device_cache="${ROOT}/build-ios-device/CMakeCache.txt"
  local sim_cache="${ROOT}/build-ios-simulator/CMakeCache.txt"
  if [[ -f "$device_cache" && -f "$sim_cache" ]]; then
    if [[ "$device_cache" -nt "$sim_cache" ]]; then
      IOS_PLATFORM=device
    else
      IOS_PLATFORM=simulator
    fi
  elif [[ -f "$device_cache" ]]; then
    IOS_PLATFORM=device
  elif [[ -f "$sim_cache" ]]; then
    IOS_PLATFORM=simulator
  fi
}

common_cmake_args() {
  # Must match PP_BROWSER_IOS_DEPLOYMENT_TARGET. Without this, Ninja stamps
  # LC_BUILD_VERSION.minos = SDK version (e.g. 18.0) and the app is killed on
  # older devices at launch (e.g. iOS 16).
  local deployment_target="${IOS_DEPLOYMENT_TARGET:-15.0}"
  local -a args=(
    -S "${ROOT}"
    -B "$(build_dir)"
    -DCMAKE_SYSTEM_NAME=iOS
    -DCMAKE_OSX_ARCHITECTURES=arm64
    -DCMAKE_OSX_DEPLOYMENT_TARGET="${deployment_target}"
    -DPP_BROWSER_IOS_DEPLOYMENT_TARGET="${deployment_target}"
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
    -DCMAKE_INSTALL_PREFIX="${INSTALL_PREFIX}"
    -DPP_BROWSER_PACKAGED_BUILD=ON
  )
  if [[ -n "${PP_BROWSER_VERSION:-}" ]]; then
    args+=(-DPP_BROWSER_VERSION="${PP_BROWSER_VERSION}")
  fi
  if [[ -n "${PP_BROWSER_RELEASE_VERSION:-}" ]]; then
    args+=(-DPP_BROWSER_RELEASE_VERSION="${PP_BROWSER_RELEASE_VERSION}")
  fi
  if [[ "${GENERATOR}" == "Xcode" ]]; then
    args+=(-G Xcode)
  else
    args+=(-G Ninja)
  fi
  printf '%s\n' "${args[@]}"
}

configure_ios() {
  require_macos
  local sysroot=""
  if [[ "${IOS_PLATFORM}" == "device" ]]; then
    sysroot=iphoneos
  else
    sysroot=iphonesimulator
  fi

  local -a cmd=(cmake)
  while IFS= read -r arg; do
    [[ -n "$arg" ]] && cmd+=("$arg")
  done < <(common_cmake_args)
  cmd+=(-DCMAKE_OSX_SYSROOT="${sysroot}")

  echo "==> Configuring iOS (${IOS_PLATFORM}, ${sysroot})"
  "${cmd[@]}"
}

cmd_build() {
  require_macos
  resolve_ios_platform
  echo "==> Building iOS (${IOS_PLATFORM}) in $(build_dir)"
  cmake --build "$(build_dir)" --config "${BUILD_TYPE}" -j
}

cmd_install() {
  require_macos
  resolve_ios_platform
  echo "==> Installing iOS (${IOS_PLATFORM}) from $(build_dir)"
  cmake --install "$(build_dir)" --config "${BUILD_TYPE}"
  local app="${INSTALL_PREFIX}/${PRODUCT_BUNDLE_NAME}.app"
  if [[ -d "$app" ]]; then
    echo "==> Installed ${app}"
  else
    echo "warning: expected ${app} — check bundle OUTPUT_NAME in cmake/IosBundle.cmake" >&2
  fi
}

pick_simulator_udid() {
  # Prefer a booted device; otherwise the newest available iPhone (else any iOS device).
  xcrun simctl list devices available -j | python3 -c "
import json, sys
raw = sys.stdin.read()
try:
    data = json.loads(raw)
except json.JSONDecodeError as e:
    sys.stderr.write('simctl JSON parse failed: %s\n' % e)
    sys.stderr.write(raw[:500] + '\n')
    raise SystemExit(1)
booted, iphones, others = [], [], []
for runtime, devices in data.get('devices', {}).items():
    rt = runtime.lower()
    if 'ios' not in rt and 'iphone' not in rt:
        continue
    for d in devices:
        if d.get('isAvailable') is False:
            continue
        entry = (runtime, d.get('name', ''), d['udid'])
        if d.get('state') == 'Booted':
            booted.append(entry)
        if 'iPhone' in d.get('name', ''):
            iphones.append(entry)
        else:
            others.append(entry)
if booted:
    print(booted[0][2])
elif iphones:
    print(iphones[-1][2])
elif others:
    print(others[-1][2])
else:
    sys.stderr.write('No iOS simulator devices. Runtimes seen: %s\n' %
                     (', '.join(data.get('devices', {}).keys()) or '(none)'))
    raise SystemExit(1)
"
}

cmd_run_sim() {
  require_macos
  local app="${INSTALL_PREFIX}/${PRODUCT_BUNDLE_NAME}.app"
  if [[ ! -d "$app" ]]; then
    echo "error: ${app} not found — run './scripts/platform/ios_build.sh sim' first" >&2
    exit 1
  fi

  local udid="${IOS_SIMULATOR_UDID:-}"
  local pick_err=""
  if [[ -z "$udid" ]]; then
    if ! udid="$(pick_simulator_udid 2>/tmp/pp-ios-sim-pick.err)"; then
      pick_err="$(cat /tmp/pp-ios-sim-pick.err 2>/dev/null || true)"
      udid=""
    fi
  fi
  if [[ -z "$udid" ]]; then
    echo "error: no available iOS simulator" >&2
    [[ -n "$pick_err" ]] && printf '%s\n' "$pick_err" >&2
    echo "hint: install a simulator runtime, then retry:" >&2
    echo "  xcodebuild -downloadPlatform iOS" >&2
    echo "  # or: Xcode → Settings → Platforms → iOS → Get" >&2
    echo "  xcrun simctl list devices available" >&2
    echo "  # optional: IOS_SIMULATOR_UDID=<udid> ./scripts/platform/ios_build.sh run-sim" >&2
    exit 1
  fi

  local state
  state="$(xcrun simctl list devices -j | python3 -c "
import json, sys
udid = sys.argv[1]
data = json.load(sys.stdin)
for devices in data.get('devices', {}).values():
    for d in devices:
        if d.get('udid') == udid:
            print(d.get('state', ''))
            raise SystemExit(0)
" "$udid" 2>/dev/null || true)"

  if [[ "$state" != "Booted" ]]; then
    echo "==> Booting simulator ${udid}"
    open -a Simulator --args -CurrentDeviceUDID "$udid"
    xcrun simctl boot "$udid" 2>/dev/null || true
    xcrun simctl bootstatus "$udid" -b
  fi

  echo "==> Installing on simulator ${udid}"
  xcrun simctl install "$udid" "$app"
  echo "==> Terminating any running ${PRODUCT_BUNDLE_NAME} instance"
  xcrun simctl terminate "$udid" dev.pp-browser.ios 2>/dev/null || true
  echo "==> Launching ${PRODUCT_BUNDLE_NAME} (--debug)"
  xcrun simctl launch "$udid" dev.pp-browser.ios --debug
  echo "==> Debug log path:"
  echo "  find ~/Library/Developer/CoreSimulator/Devices/${udid}/data/Containers/Data/Application -name pp-browser-debug.log -exec cat {} \\;"
}

pick_physical_device_udid() {
  xcrun xctrace list devices 2>/dev/null | python3 -c "
import re, sys
text = sys.stdin.read()
# Prefer lines like: Name (iOS ver) (UDID) that are not Simulator
for line in text.splitlines():
    if 'Simulator' in line:
        continue
    m = re.search(r'\(([0-9A-Fa-f-]{25,})\)\s*$', line.strip())
    if not m:
        continue
    udid = m.group(1)
    # Skip Mac hosts (no iOS version in name typically uses different shape)
    if 'Mac' in line and 'iPhone' not in line and 'iPad' not in line:
        continue
    if 'iPhone' in line or 'iPad' in line or re.search(r'\(\d+\.\d+', line):
        print(udid)
        raise SystemExit(0)
raise SystemExit(1)
"
}

physical_device_connected() {
  local want="$1"
  xcrun xctrace list devices 2>/dev/null | grep -F "$want" >/dev/null
}

coredevice_sees_udid() {
  local want="$1"
  # CoreDevice lists UDID in Hostname (…coredevice.local) and/or Identifier column.
  xcrun devicectl list devices 2>/dev/null | grep -F "$want" >/dev/null
}

cmd_run_device() {
  require_macos
  local app="${INSTALL_PREFIX}/${PRODUCT_BUNDLE_NAME}.app"
  if [[ ! -d "$app" ]]; then
    echo "error: ${app} not found — run './scripts/platform/ios_build.sh device' first" >&2
    exit 1
  fi

  local signing_env="${ROOT}/packaging/ios/signing.env"
  if [[ -f "$signing_env" ]]; then
    # shellcheck disable=SC1090
    set -a
    # shellcheck disable=SC1091
    source "$signing_env"
    set +a
  fi

  if [[ -x "${ROOT}/scripts/platform/ios_sign.sh" ]]; then
    echo "==> Signing ${app}"
    "${ROOT}/scripts/platform/ios_sign.sh" sign-app "$app"
  fi

  local udid="${IOS_DEVICE_UDID:-}"
  if [[ -n "$udid" ]] && ! physical_device_connected "$udid"; then
    echo "warning: preferred IOS_DEVICE_UDID ${udid} is not connected" >&2
    echo "hint: plug it in, or update packaging/ios/signing.env; connected:" >&2
    xcrun xctrace list devices 2>/dev/null | grep -v Simulator | grep -E 'iPhone|iPad' >&2 || true
    local fallback=""
    if fallback="$(pick_physical_device_udid)"; then
      echo "warning: falling back to connected device ${fallback}" >&2
      udid="$fallback"
    else
      echo "error: no connected physical iPhone/iPad found" >&2
      exit 1
    fi
  elif [[ -z "$udid" ]]; then
    if ! udid="$(pick_physical_device_udid)"; then
      echo "error: no connected physical iPhone/iPad found" >&2
      echo "hint: unlock the phone, trust this Mac, then: xcrun xctrace list devices" >&2
      exit 1
    fi
  fi

  local bundle_id="${IOS_BUNDLE_IDENTIFIER:-dev.pp-browser.ios}"
  echo "==> Installing on device ${udid}"

  # Newer phones (iOS 17+) pair via CoreDevice — prefer devicectl there.
  # ios-deploy is better for older USB devices that CoreDevice may not see.
  if coredevice_sees_udid "$udid"; then
    if ! xcrun devicectl device install app --device "$udid" "$app"; then
      echo "error: devicectl install failed for ${udid}" >&2
      echo "hint: if you saw 'developer disk image could not be mounted', this Mac's Xcode is older than the phone's iOS — update Xcode (and macOS if required), or plug in an older test phone and set IOS_DEVICE_UDID in packaging/ios/signing.env" >&2
      exit 1
    fi
    xcrun devicectl device process launch --device "$udid" --terminate-existing "$bundle_id" || true
  elif command -v ios-deploy >/dev/null 2>&1; then
    ios-deploy --id "$udid" --bundle "$app" --justlaunch
  else
    echo "error: could not install — need ios-deploy (brew install ios-deploy) or a CoreDevice-paired phone" >&2
    exit 1
  fi
  echo "==> Installed and launched ${bundle_id} on ${udid}"
}

cmd_clean() {
  rm -rf "${ROOT}/build-ios-simulator" "${ROOT}/build-ios-device" "${ROOT}/build-ios-xcode"
}

load_signing_env() {
  local signing_env="${ROOT}/packaging/ios/signing.env"
  if [[ -f "$signing_env" ]]; then
    # shellcheck disable=SC1090
    set -a
    # shellcheck disable=SC1091
    source "$signing_env"
    set +a
  fi
}

# Release device .app → distribution-signed IPA under dist-ios/ (TestFlight prep).
cmd_ipa() {
  require_macos
  load_signing_env

  if [[ -z "${IOS_EXPORT_METHOD:-}" ]]; then
    IOS_EXPORT_METHOD=app-store
  fi
  if [[ "${IOS_EXPORT_METHOD}" != "app-store" && "${IOS_EXPORT_METHOD}" != "ad-hoc" ]]; then
    echo "error: ipa expects IOS_EXPORT_METHOD=app-store (or ad-hoc), got ${IOS_EXPORT_METHOD}" >&2
    echo "hint: set distribution vars in packaging/ios/signing.env — see signing.env.example" >&2
    exit 1
  fi
  if [[ -z "${IOS_DISTRIBUTION_SIGNING_IDENTITY:-}" ]]; then
    echo "error: IOS_DISTRIBUTION_SIGNING_IDENTITY required for IPA export" >&2
    exit 1
  fi
  if [[ -z "${IOS_DISTRIBUTION_PROVISIONING_PROFILE_PATH:-}" ]]; then
    echo "error: IOS_DISTRIBUTION_PROVISIONING_PROFILE_PATH required for IPA export" >&2
    exit 1
  fi

  BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}"
  CMAKE_BUILD_TYPE="${BUILD_TYPE}"
  export IOS_EXPORT_METHOD
  export PP_BROWSER_VERSION="${PP_BROWSER_VERSION:-0.1.0}"
  if [[ -z "${PP_BROWSER_BUILD_NUMBER:-}" ]]; then
    # Unique-enough default so a first upload is not stuck on literal 0.1.0 forever.
    PP_BROWSER_BUILD_NUMBER="$(date +%Y%m%d%H%M)"
    export PP_BROWSER_BUILD_NUMBER
    echo "==> PP_BROWSER_BUILD_NUMBER unset — using ${PP_BROWSER_BUILD_NUMBER}"
  fi

  IOS_PLATFORM=device
  IOS_PLATFORM_EXPLICIT=1
  configure_ios
  cmd_build
  cmd_install

  local app="${INSTALL_PREFIX}/${PRODUCT_BUNDLE_NAME}.app"
  echo "==> Exporting IPA (${IOS_EXPORT_METHOD})"
  "${ROOT}/scripts/platform/ios_sign.sh" export-ipa "$app"
  echo "==> Next: ./scripts/platform/ios_build.sh upload-ipa"
  echo "    or open dist-ios/ in Transporter"
  echo "    then App Store Connect → build → export compliance → Internal Testing"
}

cmd_upload_ipa() {
  require_macos
  load_signing_env
  local ipa="${1:-${ROOT}/dist-ios/pp-browser.ipa}"
  if [[ ! -f "$ipa" ]]; then
    echo "error: IPA not found: ${ipa}" >&2
    echo "hint: run ./scripts/platform/ios_build.sh ipa first" >&2
    exit 1
  fi
  "${ROOT}/scripts/platform/ios_sign.sh" upload-ipa "$ipa"
}

main() {
  local cmd="${1:-}"
  case "${cmd}" in
    configure-sim)
      IOS_PLATFORM=simulator
      IOS_PLATFORM_EXPLICIT=1
      configure_ios
      ;;
    configure-device)
      IOS_PLATFORM=device
      IOS_PLATFORM_EXPLICIT=1
      configure_ios
      ;;
    build)
      cmd_build
      ;;
    install)
      cmd_install
      ;;
    sim)
      IOS_PLATFORM=simulator
      IOS_PLATFORM_EXPLICIT=1
      configure_ios
      cmd_build
      cmd_install
      ;;
    device)
      IOS_PLATFORM=device
      IOS_PLATFORM_EXPLICIT=1
      configure_ios
      cmd_build
      cmd_install
      ;;
    run-sim)
      cmd_run_sim
      ;;
    run-device)
      cmd_run_device
      ;;
    ipa)
      cmd_ipa
      ;;
    upload-ipa)
      shift
      cmd_upload_ipa "${1:-}"
      ;;
    xcode)
      IOS_PLATFORM=simulator
      IOS_PLATFORM_EXPLICIT=1
      GENERATOR=Xcode
      configure_ios
      echo "==> Open $(build_dir)/pp-browser.xcodeproj in Xcode (if generated)"
      ;;
    clean)
      cmd_clean
      ;;
    -h|--help|help|"")
      usage
      [[ -n "${cmd}" ]] || exit 1
      ;;
    *)
      echo "error: unknown command '${cmd}'" >&2
      usage
      exit 1
      ;;
  esac
}

main "$@"
