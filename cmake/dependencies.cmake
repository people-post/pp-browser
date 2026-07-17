include(Progress)

set(PP_THIRD_PARTY_DIR "${CMAKE_SOURCE_DIR}/third_party")

function(pp_require_vendored name)
  if(NOT EXISTS "${PP_THIRD_PARTY_DIR}/${name}/CMakeLists.txt")
    message(FATAL_ERROR
      "Missing vendored dependency '${name}' under third_party/.\n"
      "  Run: ./scripts/vendor_import.sh")
  endif()
endfunction()

pp_require_vendored(freetype)
pp_require_vendored(nlohmann_json)
pp_require_vendored(curl)
pp_require_vendored(sdl3)
pp_require_vendored(sdl3_image)
pp_require_vendored(lunasvg)
pp_require_vendored(sqlite)
pp_require_vendored(libsodium)

# libp2p deps (BoringSSL must be available before curl TLS on Linux)
include(libp2p_dependencies)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# FreeType (RmlUi font engine)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)

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

# nlohmann-json (header-only)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)

add_subdirectory("${PP_THIRD_PARTY_DIR}/nlohmann_json"
                 "${CMAKE_BINARY_DIR}/third_party/nlohmann_json" EXCLUDE_FROM_ALL)

# SQLite amalgamation (chat-storage SqliteThreadStore — pp_base, not libp2p fork)
add_subdirectory("${PP_THIRD_PARTY_DIR}/sqlite"
                 "${CMAKE_BINARY_DIR}/third_party/sqlite" EXCLUDE_FROM_ALL)

# libsodium (e2e-message-crypto symmetric layer — base/crypto)
set(SODIUM_MINIMAL OFF CACHE BOOL "" FORCE)
add_subdirectory("${PP_THIRD_PARTY_DIR}/libsodium"
                 "${CMAKE_BINARY_DIR}/third_party/libsodium" EXCLUDE_FROM_ALL)

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

add_subdirectory("${PP_THIRD_PARTY_DIR}/curl"
                 "${CMAKE_BINARY_DIR}/third_party/curl" EXCLUDE_FROM_ALL)

option(PP_BROWSER_HEADLESS "Build SDL3 without desktop video (compile-only, no GUI)" OFF)

if(PP_BROWSER_HEADLESS)
  set(SDL_UNIX_CONSOLE_BUILD ON CACHE BOOL "SDL console build without windowing" FORCE)
  message(STATUS "pp-browser: PP_BROWSER_HEADLESS=ON (no GUI)")
elseif(ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(SDL_UNIX_CONSOLE_BUILD OFF CACHE BOOL "SDL console build without windowing" FORCE)
  set(SDL_OPENGL OFF CACHE BOOL "Desktop OpenGL/GLX disabled on mobile" FORCE)
  set(SDL_OPENGLES ON CACHE BOOL "OpenGL ES for mobile SDL video" FORCE)
elseif(UNIX AND NOT APPLE)
  if(NOT EXISTS "/usr/include/X11/Xlib.h")
    message(FATAL_ERROR
      "X11 development headers are required for the pp-browser GUI.\n"
      "  Debian/Ubuntu: sudo apt install libx11-dev libxext-dev libxcursor-dev libxinerama-dev libxi-dev libxrandr-dev libxfixes-dev\n"
      "  Or configure with -DPP_BROWSER_HEADLESS=ON for compile-only builds.")
  endif()
  if(NOT EXISTS "/usr/include/GL/gl.h")
    message(FATAL_ERROR
      "OpenGL development headers are required (RmlUi uses OpenGL 3.3).\n"
      "  Debian/Ubuntu: sudo apt install libgl-dev\n"
      "  Or configure with -DPP_BROWSER_HEADLESS=ON for compile-only builds.")
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
set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
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
pp_configure_status("SDL3 configured; starting SDL3_image...")

# SDL3_image: stb for PNG/JPG (matches FetchContent on Linux); external/ codecs vendored for MSVC.
set(SDLIMAGE_BACKEND_STB ON CACHE BOOL "" FORCE)
set(SDLIMAGE_BACKEND_WIC OFF CACHE BOOL "" FORCE)
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
