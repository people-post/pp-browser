#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ANDROID_DIR="${ROOT}/android"

require_env() {
  local name="$1"
  if [[ -z "${!name:-}" ]]; then
    echo "error: ${name} is not set" >&2
    exit 1
  fi
}

# PB-046: the wrapper jar is code every build runs; it must be the official one for the pinned
# Gradle (gradle-wrapper.properties pins the distribution's checksum). Gradle publishes the jar's at
# https://services.gradle.org/distributions/gradle-<version>-wrapper.jar.sha256 — update both together.
GRADLE_WRAPPER_JAR_SHA256="2db75c40782f5e8ba1fc278a5574bab070adccb2d21ca5a6e5ed840888448046"  # 8.12

verify_gradle_wrapper() {
  local jar="${ANDROID_DIR}/gradle/wrapper/gradle-wrapper.jar" actual
  if command -v sha256sum >/dev/null 2>&1; then
    actual="$(sha256sum "${jar}" | cut -d' ' -f1)"
  else
    actual="$(shasum -a 256 "${jar}" | cut -d' ' -f1)"
  fi
  if [[ "${actual}" != "${GRADLE_WRAPPER_JAR_SHA256}" ]]; then
    echo "error: ${jar} is not the official Gradle wrapper (sha256 ${actual})" >&2
    exit 1
  fi
}

# Gradle reads PP_RELAY_CONTENT_SHA256 from the environment; reject anything but ON/OFF here.
check_relay_content_sha256() {
  local v
  v="$(printf '%s' "${PP_RELAY_CONTENT_SHA256:-OFF}" | tr '[:lower:]' '[:upper:]')"
  if [[ "${v}" != "ON" && "${v}" != "OFF" ]]; then
    echo "error: PP_RELAY_CONTENT_SHA256 must be ON or OFF (got '${PP_RELAY_CONTENT_SHA256}')" >&2
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
  check_relay_content_sha256
  verify_gradle_wrapper
  # ${arr[@]+"${arr[@]}"}: an empty array trips `set -u` on bash 3.2 (macOS /bin/bash).
  ./gradlew "$task" ${version_args[@]+"${version_args[@]}"}
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
  PP_API_URL_PROD             Prod backend origin compiled into the app (read by Gradle; empty = none)
  PP_API_URL_QA               QA backend origin compiled into the app (empty = QA option hidden)
  PP_RELAY_CONTENT_SHA256     ON/OFF: default of relay.content_sha256 (x-amz-content-sha256 on relay POST/PUT); empty = OFF

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
    check_relay_content_sha256
    verify_gradle_wrapper
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
