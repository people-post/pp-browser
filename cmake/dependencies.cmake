include(FetchContent)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# FreeType (RmlUi font engine)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)

FetchContent_Declare(
  freetype
  GIT_REPOSITORY https://github.com/freetype/freetype.git
  GIT_TAG VER-2-13-3
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(freetype)

if(NOT TARGET Freetype::Freetype)
  if(TARGET freetype-interface)
    add_library(Freetype::Freetype ALIAS freetype-interface)
  elseif(TARGET freetype)
    add_library(Freetype::Freetype ALIAS freetype)
  else()
    message(FATAL_ERROR "FreeType target not found after FetchContent")
  endif()
endif()

# nlohmann-json (header-only)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(json)

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
set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)

if(WIN32)
  set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
elseif(APPLE)
  set(CURL_USE_SECTRANSP ON CACHE BOOL "" FORCE)
else()
  set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
  find_package(OpenSSL REQUIRED)
endif()

FetchContent_Declare(
  curl
  GIT_REPOSITORY https://github.com/curl/curl.git
  GIT_TAG curl-8_11_1
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(curl)

option(PP_BROWSER_HEADLESS "Build SDL3 without desktop video (compile-only, no GUI)" OFF)

if(PP_BROWSER_HEADLESS)
  set(SDL_UNIX_CONSOLE_BUILD ON CACHE BOOL "SDL console build without windowing" FORCE)
  message(STATUS "pp-browser: PP_BROWSER_HEADLESS=ON (no GUI)")
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
set(SDL_X11 ON CACHE BOOL "" FORCE)

FetchContent_Declare(
  SDL3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG release-3.2.8
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(SDL3)

set(SDL3IMAGE_AVIF OFF CACHE BOOL "" FORCE)
set(SDL3IMAGE_BMP ON CACHE BOOL "" FORCE)
set(SDL3IMAGE_JPG ON CACHE BOOL "" FORCE)
set(SDL3IMAGE_PNG ON CACHE BOOL "" FORCE)

FetchContent_Declare(
  SDL3_image
  GIT_REPOSITORY https://github.com/libsdl-org/SDL_image.git
  GIT_TAG release-3.2.4
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(SDL3_image)

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
endif()

if(NOT TARGET SDL_image::SDL_image)
  add_library(SDL_image_alias INTERFACE)
  add_library(SDL_image::SDL_image ALIAS SDL_image_alias)
  target_link_libraries(SDL_image_alias INTERFACE ${PP_BROWSER_SDL3_IMAGE_TARGET})
endif()
