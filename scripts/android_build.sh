#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ANDROID_DIR="${ROOT}/android"

require_env() {
  local name="$1"
  if [[ -z "${!name:-}" ]]; then
    echo "error: ${name} is not set" >&2
    exit 1
  fi
}

run_gradlew() {
  local task="$1"
  local -a version_args=()
  if [[ -n "${PP_BROWSER_VERSION:-}" ]]; then
    version_args+=("-PppBrowserVersion=${PP_BROWSER_VERSION}")
  fi
  if [[ -n "${PP_BROWSER_RELEASE_VERSION:-}" ]]; then
    version_args+=("-PppBrowserReleaseVersion=${PP_BROWSER_RELEASE_VERSION}")
  fi
  ./gradlew "$task" "${version_args[@]}"
}

usage() {
  cat <<EOF
Usage: $(basename "$0") <command>

Commands:
  apk            Build debug APK (assembleDebug)
  apk-release    Build release APK (assembleRelease)
  install        Build and install debug APK on a connected device/emulator
                   Set ANDROID_SERIAL to target one device when several are connected.
  configure      Optional: run a standalone CMake configure for Android (sanity check)

Optional environment variables:
  PP_BROWSER_VERSION          Numeric version passed to CMake (e.g. 0.1.0)
  PP_BROWSER_RELEASE_VERSION  Full release version (e.g. 0.1.0-rc1)

Requires:
  ANDROID_SDK_ROOT (or ANDROID_HOME)
  ANDROID_NDK_HOME
  JDK 17+
EOF
}

cmd="${1:-}"
case "${cmd}" in
  apk)
    require_env ANDROID_SDK_ROOT
    require_env ANDROID_NDK_HOME
    cd "${ANDROID_DIR}"
    run_gradlew assembleDebug
    ;;
  apk-release)
    require_env ANDROID_SDK_ROOT
    require_env ANDROID_NDK_HOME
    cd "${ANDROID_DIR}"
    run_gradlew assembleRelease
    ;;
  install)
    require_env ANDROID_SDK_ROOT
    require_env ANDROID_NDK_HOME
    cd "${ANDROID_DIR}"
    if [[ -n "${ANDROID_SERIAL:-}" ]]; then
      ./gradlew installDebug -Pandroid.injected.invoked.from.ide=true \
        -Pandroid.injected.device.serial="${ANDROID_SERIAL}"
    else
      ./gradlew installDebug
    fi
    ;;
  configure)
    require_env ANDROID_NDK_HOME
    local_abi="${ANDROID_ABI:-arm64-v8a}"
    cmake -B "${ROOT}/build-android-${local_abi}" -S "${ROOT}" \
      -DCMAKE_TOOLCHAIN_FILE="${ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake" \
      -DANDROID_ABI="${local_abi}" \
      -DANDROID_PLATFORM=android-24 \
      -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON \
      -DCMAKE_BUILD_TYPE=Debug
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
