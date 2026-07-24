#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

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
  run-sim           Boot a simulator if needed, install Frame.app, and launch
  xcode             Configure with -G Xcode (open in Xcode for debugging)
  clean             Remove build-ios-* directories

Environment:
  IOS_PLATFORM              simulator (default) or device
  CMAKE_BUILD_TYPE          Debug (default) or Release
  INSTALL_PREFIX            Output prefix for cmake --install
  PP_BROWSER_VERSION        Passed to CMake (e.g. 0.1.0)
  PP_BROWSER_RELEASE_VERSION  Full version string (e.g. 0.1.0-rc1)
  IOS_CMAKE_GENERATOR       Ninja (default) or Xcode
  IOS_SIMULATOR_UDID        Target a specific simulator (optional; otherwise newest iPhone)

Requires (macOS only):
  Xcode 15+ with iOS SDK
  CMake 3.24+
  Ninja (recommended)
  Perl (lsquic codegen)

Signing (device / TestFlight): see packaging/ios/signing.env.example and scripts/ios_sign.sh
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

common_cmake_args() {
  local -a args=(
    -S "${ROOT}"
    -B "$(build_dir)"
    -DCMAKE_SYSTEM_NAME=iOS
    -DCMAKE_OSX_ARCHITECTURES=arm64
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
  cmake --build "$(build_dir)" --config "${BUILD_TYPE}" -j
}

cmd_install() {
  require_macos
  cmake --install "$(build_dir)" --config "${BUILD_TYPE}"
  local app="${INSTALL_PREFIX}/Frame.app"
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
  local app="${INSTALL_PREFIX}/Frame.app"
  if [[ ! -d "$app" ]]; then
    echo "error: ${app} not found — run './scripts/ios_build.sh sim' first" >&2
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
    echo "  # optional: IOS_SIMULATOR_UDID=<udid> ./scripts/ios_build.sh run-sim" >&2
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
  echo "==> Terminating any running Frame instance"
  xcrun simctl terminate "$udid" dev.pp-browser.ios 2>/dev/null || true
  echo "==> Launching Frame (--debug)"
  xcrun simctl launch "$udid" dev.pp-browser.ios --debug
  echo "==> Debug log path:"
  echo "  find ~/Library/Developer/CoreSimulator/Devices/${udid}/data/Containers/Data/Application -name pp-browser-debug.log -exec cat {} \\;"
}

cmd_clean() {
  rm -rf "${ROOT}/build-ios-simulator" "${ROOT}/build-ios-device" "${ROOT}/build-ios-xcode"
}

main() {
  local cmd="${1:-}"
  case "${cmd}" in
    configure-sim)
      IOS_PLATFORM=simulator
      configure_ios
      ;;
    configure-device)
      IOS_PLATFORM=device
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
      configure_ios
      cmd_build
      cmd_install
      ;;
    device)
      IOS_PLATFORM=device
      configure_ios
      cmd_build
      cmd_install
      ;;
    run-sim)
      cmd_run_sim
      ;;
    xcode)
      IOS_PLATFORM=simulator
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
