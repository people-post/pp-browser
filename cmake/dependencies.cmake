include(Progress)

set(PP_THIRD_PARTY_DIR "${CMAKE_SOURCE_DIR}/third_party")

function(pp_require_vendored name)
  if(NOT EXISTS "${PP_THIRD_PARTY_DIR}/${name}/CMakeLists.txt")
    message(FATAL_ERROR
      "Missing vendored dependency '${name}' under third_party/.\n"
      "  Run: ./scripts/vendor_import.sh")
  endif()
endfunction()

# Always required for pp-node and the GUI app.
# JSON document tree comes from FetchContent pp-cpp-common (Value/Object).
# libsodium + ML-KEM/ML-DSA come from FetchContent pp-cpp-crypto (pinned tag).

# GUI / AI / messaging / A-V — not needed for headless pp-node.
# FreeType / HarfBuzz / LunaSVG / SDL3 / SDL3_image come from pp-cpp-ui.
# curl is required for headless too (pp-node mesh directory publish — N027).
pp_require_vendored(curl)
if(NOT PP_BROWSER_HEADLESS)
  pp_require_vendored(sqlite)
  pp_require_vendored(opus)
endif()

# libp2p deps (BoringSSL must be available before curl TLS on Linux)
include(libp2p_dependencies)

if(NOT PP_BROWSER_IS_MOBILE)
  pp_require_vendored(miniupnpc)
  set(UPNPC_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  set(UPNPC_BUILD_STATIC ON CACHE BOOL "" FORCE)
  set(UPNPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(UPNPC_BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
  set(UPNPC_NO_INSTALL ON CACHE BOOL "" FORCE)
  add_subdirectory("${PP_THIRD_PARTY_DIR}/miniupnpc"
                 "${CMAKE_BINARY_DIR}/third_party/miniupnpc" EXCLUDE_FROM_ALL)
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# libcurl — headless pp-node + GUI (LLM / Brief HTTP)
set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_LDAPS ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_TELNET ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_DICT ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_FILE ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_TFTP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_RTSP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_POP3 ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_IMAP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_SMTP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_GOPHER ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_MQTT ON CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
set(CURL_ZLIB OFF CACHE BOOL "" FORCE)
set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
set(CURL_ENABLE_EXPORT_TARGET OFF CACHE BOOL "" FORCE)
set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
# Do not auto-link Homebrew/system nghttp2 (breaks Developer ID / notarized apps).
set(USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
set(USE_NGTCP2 OFF CACHE BOOL "" FORCE)
set(USE_QUICHE OFF CACHE BOOL "" FORCE)
set(USE_OPENSSL_QUIC OFF CACHE BOOL "" FORCE)
# Avoid pkg-config picking Cellar dylibs for optional curl deps.
set(CURL_USE_PKGCONFIG OFF CACHE BOOL "" FORCE)

if(WIN32)
  set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
elseif(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(CURL_USE_SECTRANSP ON CACHE BOOL "" FORCE)
else()
  set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
  list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/modules")
  set(HAVE_BORINGSSL TRUE CACHE BOOL "" FORCE)
  set(HAVE_SSL_SET0_WBIO TRUE CACHE BOOL "" FORCE)
  set(HAVE_OPENSSL_SRP FALSE CACHE BOOL "" FORCE)
  set(HAVE_ECH FALSE CACHE BOOL "" FORCE)
  find_package(OpenSSL REQUIRED)
endif()

if(ANDROID)
  set(CURL_CA_BUNDLE "none" CACHE STRING "" FORCE)
  set(CURL_CA_PATH "/system/etc/security/cacerts" CACHE STRING "" FORCE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(CURL_CA_BUNDLE "none" CACHE STRING "" FORCE)
  set(CURL_CA_PATH "none" CACHE STRING "" FORCE)
endif()

add_subdirectory("${PP_THIRD_PARTY_DIR}/curl"
                 "${CMAKE_BINARY_DIR}/third_party/curl" EXCLUDE_FROM_ALL)

if(PP_BROWSER_HEADLESS)
  pp_configure_status("Headless deps ready (pp-cpp-common, pp-cpp-crypto, libp2p, curl); skipping GUI third_party")
  return()
endif()

# --- GUI / full-app third_party below ---

# FreeType / HarfBuzz / LunaSVG (+ zlib/libpng for CBDT) come from pp-cpp-ui.

# SQLite amalgamation (chat-storage SqliteThreadStore — pp_base, not libp2p fork)
add_subdirectory("${PP_THIRD_PARTY_DIR}/sqlite"
                 "${CMAKE_BINARY_DIR}/third_party/sqlite" EXCLUDE_FROM_ALL)

# Opus (p2p-av-calls a2 voice)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
add_subdirectory("${PP_THIRD_PARTY_DIR}/opus"
                 "${CMAKE_BINARY_DIR}/third_party/opus" EXCLUDE_FROM_ALL)

# SDL3 + SDL3_image come from pp-cpp-ui (included after this file for non-headless).
# PP_BROWSER_SDL3_* aliases are set in cmake/PpCppUi.cmake.

if(UNIX AND NOT APPLE AND NOT ANDROID AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
  find_package(PkgConfig QUIET)
  set(_pp_pulse_ok FALSE)
  set(_pp_alsa_ok FALSE)
  if(PkgConfig_FOUND)
    pkg_check_modules(PP_SDL_PULSE QUIET libpulse)
    pkg_check_modules(PP_SDL_ALSA QUIET alsa)
    if(PP_SDL_PULSE_FOUND)
      set(_pp_pulse_ok TRUE)
    endif()
    if(PP_SDL_ALSA_FOUND)
      set(_pp_alsa_ok TRUE)
    endif()
  endif()
  if(NOT _pp_pulse_ok OR NOT _pp_alsa_ok)
    message(WARNING
      "Linux A/V calls need PulseAudio + ALSA development packages for real mic/speaker.\n"
      "  Without them SDL falls back to dummy audio (no capture/playback).\n"
      "  Debian/Ubuntu: sudo apt install libpulse-dev libasound2-dev\n"
      "  Then reconfigure; if drivers stay dummy: rm -rf build/_deps/pp_cpp_ui-build/third_party/sdl3 && cmake -B build -S .")
  else()
    message(STATUS "pp-browser: SDL audio backends — PulseAudio + ALSA (dev packages found)")
  endif()
endif()

pp_configure_status("All third_party dependencies ready (SDL via PpCppUi next)")
