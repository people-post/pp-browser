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
pp_require_vendored(nlohmann_json)
pp_require_vendored(libsodium)

# GUI / AI / messaging / A-V — not needed for headless pp-node.
if(NOT PP_BROWSER_HEADLESS)
  pp_require_vendored(freetype)
  pp_require_vendored(harfbuzz)
  pp_require_vendored(curl)
  pp_require_vendored(sdl3)
  pp_require_vendored(sdl3_image)
  pp_require_vendored(lunasvg)
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

# nlohmann-json (header-only)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)

add_subdirectory("${PP_THIRD_PARTY_DIR}/nlohmann_json"
                 "${CMAKE_BINARY_DIR}/third_party/nlohmann_json" EXCLUDE_FROM_ALL)

# libsodium (e2e-message-crypto symmetric layer — base/crypto)
set(SODIUM_MINIMAL OFF CACHE BOOL "" FORCE)
add_subdirectory("${PP_THIRD_PARTY_DIR}/libsodium"
                 "${CMAKE_BINARY_DIR}/third_party/libsodium" EXCLUDE_FROM_ALL)

if(PP_BROWSER_HEADLESS)
  pp_configure_status("Headless deps ready (json, sodium, libp2p); skipping GUI third_party")
  return()
endif()

# --- GUI / full-app third_party below ---

# FreeType (RmlUi font engine) — PNG required for Noto Color Emoji CBDT bitmaps.
# Prefer system zlib/libpng when present; otherwise use SDL3_image vendored copies.
# On Windows, never trust PATH-discovered MinGW archives (e.g. Strawberry Perl):
# they are usually the wrong arch/ABI for MSVC (x64 .a linked into ARM64/x64 MSVC).
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG OFF CACHE BOOL "" FORCE)
set(FT_REQUIRE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_ZLIB OFF CACHE BOOL "" FORCE)
set(FT_REQUIRE_ZLIB ON CACHE BOOL "" FORCE)

if(WIN32)
  set(ZLIB_FOUND FALSE)
  set(PNG_FOUND FALSE)
else()
  find_package(ZLIB QUIET)
  find_package(PNG QUIET)
endif()
if(NOT ZLIB_FOUND OR NOT PNG_FOUND)
  message(STATUS "pp-browser: vendoring zlib+libpng for FreeType color emoji")
  # Drop stale FindZLIB/FindPNG cache entries that may name incompatible system libs.
  unset(ZLIB_LIBRARY CACHE)
  unset(ZLIB_LIBRARY_RELEASE CACHE)
  unset(ZLIB_LIBRARY_DEBUG CACHE)
  unset(PNG_LIBRARY CACHE)
  unset(PNG_LIBRARY_RELEASE CACHE)
  unset(PNG_LIBRARY_DEBUG CACHE)
  unset(PNG_PNG_INCLUDE_DIR CACHE)
  set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  # libp2p_dependencies may already have added third_party/zlib as target `zlib`.
  # Reuse it (and its headers) so libpng's PNG_ZLIB_VERNUM matches the compile
  # include path; do not mix sdl3_image zlib headers with the libp2p zlib binary.
  set(_pp_added_zlib_ft FALSE)
  if(NOT TARGET zlibstatic AND NOT TARGET zlib)
    add_subdirectory("${PP_THIRD_PARTY_DIR}/sdl3_image/external/zlib"
                     "${CMAKE_BINARY_DIR}/third_party/zlib_ft" EXCLUDE_FROM_ALL)
    set(_pp_added_zlib_ft TRUE)
  endif()
  if(TARGET zlibstatic)
    set(_pp_zlib_lib zlibstatic)
  else()
    set(_pp_zlib_lib zlib)
  endif()
  if(_pp_added_zlib_ft)
    set(_pp_zlib_inc
        "${PP_THIRD_PARTY_DIR}/sdl3_image/external/zlib;${CMAKE_BINARY_DIR}/third_party/zlib_ft")
  else()
    set(_pp_zlib_inc
        "${PP_THIRD_PARTY_DIR}/zlib;${CMAKE_BINARY_DIR}/third_party/zlib")
  endif()
  set(ZLIB_LIBRARY ${_pp_zlib_lib} CACHE FILEPATH "zlib for FreeType/libpng" FORCE)
  set(ZLIB_INCLUDE_DIR "${_pp_zlib_inc}"
      CACHE PATH "zlib include for FreeType/libpng" FORCE)
  set(ZLIB_FOUND TRUE CACHE BOOL "" FORCE)
  set(ZLIB_LIBRARIES ${ZLIB_LIBRARY})
  set(ZLIB_INCLUDE_DIRS ${ZLIB_INCLUDE_DIR})
  if(NOT TARGET ZLIB::ZLIB)
    add_library(ZLIB::ZLIB ALIAS ${_pp_zlib_lib})
  endif()

  set(PNG_SHARED OFF CACHE BOOL "" FORCE)
  set(PNG_STATIC ON CACHE BOOL "" FORCE)
  set(PNG_TESTS OFF CACHE BOOL "" FORCE)
  set(PNG_TOOLS OFF CACHE BOOL "" FORCE)
  set(PNG_FRAMEWORK OFF CACHE BOOL "" FORCE)
  set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
  # libpng's project(... LANGUAGES C ASM) needs CMAKE_ASM_COMPILE_OBJECT at
  # generate time. On WIN32 the root project() declares ASM (+ MSVC stub rule).
  if(MSVC AND NOT CMAKE_ASM_COMPILE_OBJECT)
    set(CMAKE_ASM_COMPILE_OBJECT
        "<CMAKE_C_COMPILER> <DEFINES> <INCLUDES> <FLAGS> /c /Fo<OBJECT> <SOURCE>")
  endif()
  if(NOT TARGET png_static AND NOT TARGET png_shared AND NOT TARGET png)
    add_subdirectory("${PP_THIRD_PARTY_DIR}/sdl3_image/external/libpng"
                     "${CMAKE_BINARY_DIR}/third_party/libpng_ft" EXCLUDE_FROM_ALL)
  endif()
  if(TARGET png_static)
    set(_pp_png_lib png_static)
  elseif(TARGET png)
    set(_pp_png_lib png)
  else()
    message(FATAL_ERROR "Vendored libpng target not found after add_subdirectory")
  endif()
  # Satisfy FreeType's find_package(PNG) via a redirect config package.
  set(_pp_png_cfg_dir "${CMAKE_BINARY_DIR}/pp_cmake_png")
  file(MAKE_DIRECTORY "${_pp_png_cfg_dir}")
  file(WRITE "${_pp_png_cfg_dir}/PNGConfig.cmake"
       "set(PNG_FOUND TRUE)\n"
       "set(PNG_INCLUDE_DIRS \"${PP_THIRD_PARTY_DIR}/sdl3_image/external/libpng\")\n"
       "set(PNG_LIBRARIES ${_pp_png_lib})\n"
       "set(PNG_LIBRARY ${_pp_png_lib})\n"
       "set(PNG_PNG_INCLUDE_DIR \"${PP_THIRD_PARTY_DIR}/sdl3_image/external/libpng\")\n"
       "if(NOT TARGET PNG::PNG)\n"
       "  add_library(PNG::PNG INTERFACE IMPORTED)\n"
       "  set_target_properties(PNG::PNG PROPERTIES\n"
       "    INTERFACE_INCLUDE_DIRECTORIES \"${PP_THIRD_PARTY_DIR}/sdl3_image/external/libpng\"\n"
       "    INTERFACE_LINK_LIBRARIES ${_pp_png_lib})\n"
       "endif()\n")
  set(_pp_zlib_cfg_dir "${CMAKE_BINARY_DIR}/pp_cmake_zlib")
  file(MAKE_DIRECTORY "${_pp_zlib_cfg_dir}")
  file(WRITE "${_pp_zlib_cfg_dir}/ZLIBConfig.cmake"
       "set(ZLIB_FOUND TRUE)\n"
       "set(ZLIB_INCLUDE_DIRS \"${ZLIB_INCLUDE_DIR}\")\n"
       "set(ZLIB_LIBRARIES ${_pp_zlib_lib})\n"
       "set(ZLIB_LIBRARY ${_pp_zlib_lib})\n"
       "set(ZLIB_INCLUDE_DIR \"${ZLIB_INCLUDE_DIR}\")\n"
       "if(NOT TARGET ZLIB::ZLIB)\n"
       "  add_library(ZLIB::ZLIB INTERFACE IMPORTED)\n"
       "  set_target_properties(ZLIB::ZLIB PROPERTIES\n"
       "    INTERFACE_INCLUDE_DIRECTORIES \"${ZLIB_INCLUDE_DIR}\"\n"
       "    INTERFACE_LINK_LIBRARIES ${_pp_zlib_lib})\n"
       "endif()\n")
  list(PREPEND CMAKE_PREFIX_PATH "${_pp_png_cfg_dir}" "${_pp_zlib_cfg_dir}")
  set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON)
  # iOS/Android set CMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY, so CMAKE_PREFIX_PATH alone
  # is ignored for find_package. Point *_DIR at our redirect configs (host paths).
  set(PNG_DIR "${_pp_png_cfg_dir}" CACHE PATH "Vendored PNG config for FreeType" FORCE)
  set(ZLIB_DIR "${_pp_zlib_cfg_dir}" CACHE PATH "Vendored ZLIB config for FreeType" FORCE)
  set(PNG_FOUND TRUE)
  set(PNG_LIBRARIES ${_pp_png_lib})
  set(PNG_INCLUDE_DIRS "${PP_THIRD_PARTY_DIR}/sdl3_image/external/libpng")
endif()

add_subdirectory("${PP_THIRD_PARTY_DIR}/freetype"
                 "${CMAKE_BINARY_DIR}/third_party/freetype" EXCLUDE_FROM_ALL)

if(NOT TARGET Freetype::Freetype)
  if(TARGET freetype-interface)
    add_library(Freetype::Freetype ALIAS freetype-interface)
  elseif(TARGET freetype)
    add_library(Freetype::Freetype ALIAS freetype)
  else()
    message(FATAL_ERROR "FreeType target not found after add_subdirectory")
  endif()
endif()

# HarfBuzz (RmlUi text shaping)
set(HB_BUILD_UTILS OFF CACHE BOOL "" FORCE)
set(HB_BUILD_SUBSET OFF CACHE BOOL "" FORCE)
set(HB_HAVE_GLIB OFF CACHE BOOL "" FORCE)
set(HB_HAVE_ICU OFF CACHE BOOL "" FORCE)
set(HB_HAVE_GRAPHITE2 OFF CACHE BOOL "" FORCE)
set(HB_HAVE_GOBJECT OFF CACHE BOOL "" FORCE)
# Vendored HarfBuzz compiles CoreText when HB_HAVE_CORETEXT (default ON on
# Apple). On iOS that requires HB_IOS so it links CoreText/CoreGraphics instead
# of ApplicationServices (macOS-only); otherwise the final app link fails with
# undefined CT* symbols.
if(PP_BROWSER_IS_IOS)
  set(HB_IOS ON CACHE BOOL "" FORCE)
endif()

add_subdirectory("${PP_THIRD_PARTY_DIR}/harfbuzz"
                 "${CMAKE_BINARY_DIR}/third_party/harfbuzz" EXCLUDE_FROM_ALL)

if(TARGET harfbuzz AND NOT TARGET harfbuzz::harfbuzz)
  add_library(harfbuzz::harfbuzz ALIAS harfbuzz)
endif()

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

# LunaSVG (RmlUi SVG plugin)
set(LUNASVG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(PLUTOVG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

add_subdirectory("${PP_THIRD_PARTY_DIR}/lunasvg"
                 "${CMAKE_BINARY_DIR}/third_party/lunasvg" EXCLUDE_FROM_ALL)

# libcurl (LLM HTTP client)
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
set(CURL_ENABLE_EXPORT_TARGET OFF CACHE BOOL "" FORCE)
set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)

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
  # Prime OpenSSL for vendored curl (it calls find_package(OpenSSL) again).
  find_package(OpenSSL REQUIRED)
endif()

# Mobile + BoringSSL: do not bake host (/etc/ssl) CA paths into curl_config.h.
# Android: runtime os::TlsCaPath() / ApplyPlatformCurlSsl sets CAPATH (apex preferred).
# iOS: ApplyPlatformCurlSsl installs a SecTrust SSL_CTX verify callback.
if(ANDROID)
  set(CURL_CA_BUNDLE "none" CACHE STRING "" FORCE)
  set(CURL_CA_PATH "/system/etc/security/cacerts" CACHE STRING "" FORCE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(CURL_CA_BUNDLE "none" CACHE STRING "" FORCE)
  set(CURL_CA_PATH "none" CACHE STRING "" FORCE)
endif()

add_subdirectory("${PP_THIRD_PARTY_DIR}/curl"
                 "${CMAKE_BINARY_DIR}/third_party/curl" EXCLUDE_FROM_ALL)

if(ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(SDL_UNIX_CONSOLE_BUILD OFF CACHE BOOL "SDL console build without windowing" FORCE)
  set(SDL_OPENGL OFF CACHE BOOL "Desktop OpenGL/GLX disabled on mobile" FORCE)
  set(SDL_OPENGLES ON CACHE BOOL "OpenGL ES for mobile SDL video" FORCE)
elseif(UNIX AND NOT APPLE)
  if(NOT EXISTS "/usr/include/X11/Xlib.h")
    message(FATAL_ERROR
      "X11 development headers are required for the pp-browser GUI.\n"
      "  Debian/Ubuntu: sudo apt install libx11-dev libxext-dev libxcursor-dev libxinerama-dev libxi-dev libxrandr-dev libxfixes-dev\n"
      "  Or configure with -DPP_BROWSER_HEADLESS=ON for pp-node / node-only builds.")
  endif()
  if(NOT EXISTS "/usr/include/GL/gl.h")
    message(FATAL_ERROR
      "OpenGL development headers are required (RmlUi uses OpenGL 3.3).\n"
      "  Debian/Ubuntu: sudo apt install libgl-dev\n"
      "  Or configure with -DPP_BROWSER_HEADLESS=ON for pp-node / node-only builds.")
  endif()
  set(SDL_UNIX_CONSOLE_BUILD OFF CACHE BOOL "SDL console build without windowing" FORCE)
  set(SDL_OPENGL ON CACHE BOOL "Include OpenGL/GLX in SDL3" FORCE)
  set(SDL_OPENGLES OFF CACHE BOOL "Disable OpenGL ES (desktop GL3 backend)" FORCE)
endif()

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_DBUS OFF CACHE BOOL "" FORCE)
set(SDL_IBUS OFF CACHE BOOL "" FORCE)
set(SDL_WAYLAND OFF CACHE BOOL "" FORCE)
if(ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(SDL_X11 OFF CACHE BOOL "" FORCE)
else()
  set(SDL_X11 ON CACHE BOOL "" FORCE)
endif()
# Audio on for a2 voice (CallMediaEngine); camera stays off until a3.
set(SDL_AUDIO ON CACHE BOOL "" FORCE)
if(UNIX AND NOT APPLE AND NOT ANDROID AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
  # Prefer Pulse (desktop mixer / PipeWire compat); ALSA as device fallback.
  set(SDL_PULSEAUDIO ON CACHE BOOL "" FORCE)
  set(SDL_ALSA ON CACHE BOOL "" FORCE)
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
      "  Then reconfigure; if drivers stay dummy: rm -rf build/third_party/sdl3 && cmake -B build -S .")
  else()
    message(STATUS "pp-browser: SDL audio backends — PulseAudio + ALSA (dev packages found)")
  endif()
endif()
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA ON CACHE BOOL "" FORCE)  # a3 video capture (V018)
set(SDL_JOYSTICK OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
if(ANDROID)
  # Android JNI registers HIDDeviceManager natives; hid.cpp must be linked.
  set(SDL_HIDAPI ON CACHE BOOL "" FORCE)
else()
  set(SDL_HIDAPI OFF CACHE BOOL "" FORCE)
endif()
set(SDL_DIALOG OFF CACHE BOOL "" FORCE)
set(SDL_VULKAN OFF CACHE BOOL "" FORCE)
set(SDL_PIPEWIRE OFF CACHE BOOL "" FORCE)
set(SDL_LIBUDEV OFF CACHE BOOL "" FORCE)
set(SDL_LIBURING OFF CACHE BOOL "" FORCE)

add_subdirectory("${PP_THIRD_PARTY_DIR}/sdl3"
                 "${CMAKE_BINARY_DIR}/third_party/sdl3" EXCLUDE_FROM_ALL)
# UIKit video events reference GCKeyboard/GCMouse even with SDL_JOYSTICK=OFF.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  if(TARGET SDL3-static)
    target_link_libraries(SDL3-static PUBLIC "$<LINK_LIBRARY:FRAMEWORK,GameController>")
  elseif(TARGET SDL3)
    target_link_libraries(SDL3 PUBLIC "$<LINK_LIBRARY:FRAMEWORK,GameController>")
  endif()
endif()
pp_configure_status("SDL3 configured; starting SDL3_image...")

# SDL3_image: stb for PNG/JPG (matches FetchContent on Linux); external/ codecs vendored for MSVC.
set(SDLIMAGE_BACKEND_STB ON CACHE BOOL "" FORCE)
set(SDLIMAGE_BACKEND_WIC OFF CACHE BOOL "" FORCE)
# ImageIO.m needs the ImageIO framework; on iOS we use STB only (same as Android).
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(SDLIMAGE_BACKEND_IMAGEIO OFF CACHE BOOL "" FORCE)
endif()
set(SDLIMAGE_AVIF OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_JXL OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_TIF OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_WEBP OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_BMP ON CACHE BOOL "" FORCE)
set(SDLIMAGE_JPG ON CACHE BOOL "" FORCE)
set(SDLIMAGE_PNG ON CACHE BOOL "" FORCE)
set(SDLIMAGE_INSTALL OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_TESTS OFF CACHE BOOL "" FORCE)

if(NOT EXISTS "${PP_THIRD_PARTY_DIR}/sdl3_image/external/libavif/CMakeLists.txt")
  message(FATAL_ERROR
    "Missing SDL3_image external codecs under third_party/sdl3_image/external/.\n"
    "  Run: ./scripts/vendor_import.sh")
endif()

add_subdirectory("${PP_THIRD_PARTY_DIR}/sdl3_image"
                 "${CMAKE_BINARY_DIR}/third_party/sdl3_image" EXCLUDE_FROM_ALL)
pp_configure_status("SDL3_image configured; finishing dependency setup...")

if(TARGET SDL3::SDL3-static)
  set(PP_BROWSER_SDL3_TARGET SDL3::SDL3-static)
elseif(TARGET SDL3::SDL3)
  set(PP_BROWSER_SDL3_TARGET SDL3::SDL3)
else()
  message(FATAL_ERROR "SDL3 target not found")
endif()

if(TARGET SDL3_image::SDL3_image-static)
  set(PP_BROWSER_SDL3_IMAGE_TARGET SDL3_image::SDL3_image-static)
elseif(TARGET SDL3_image::SDL3_image)
  set(PP_BROWSER_SDL3_IMAGE_TARGET SDL3_image::SDL3_image)
else()
  message(FATAL_ERROR "SDL3_image target not found")
endif()

if(NOT TARGET SDL::SDL)
  add_library(SDL_alias INTERFACE)
  add_library(SDL::SDL ALIAS SDL_alias)
  target_link_libraries(SDL_alias INTERFACE ${PP_BROWSER_SDL3_TARGET})
  target_compile_definitions(SDL_alias INTERFACE RMLUI_SDL_VERSION_MAJOR=3)
endif()

if(NOT TARGET SDL_image::SDL_image)
  add_library(SDL_image_alias INTERFACE)
  add_library(SDL_image::SDL_image ALIAS SDL_image_alias)
  target_link_libraries(SDL_image_alias INTERFACE ${PP_BROWSER_SDL3_IMAGE_TARGET})
endif()

pp_configure_status("All third_party dependencies ready")
