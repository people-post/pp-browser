# Platform detection and mobile defaults for pp-browser.

# Headless / node-only: skip SDL, RmlUi, and GUI platform (pp-node packaging).
# Declared here so dependencies.cmake and all src/*/CMakeLists.txt can gate on it.
option(PP_BROWSER_HEADLESS
  "Headless/node-only build: skip SDL, RmlUi, and GUI modules (pp-node)"
  OFF)

set(PP_BROWSER_IS_ANDROID FALSE)
set(PP_BROWSER_IS_IOS FALSE)
set(PP_BROWSER_IS_MOBILE FALSE)

if(ANDROID)
  set(PP_BROWSER_IS_ANDROID TRUE)
  set(PP_BROWSER_IS_MOBILE TRUE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(PP_BROWSER_IS_IOS TRUE)
  set(PP_BROWSER_IS_MOBILE TRUE)
endif()

if(PP_BROWSER_HEADLESS)
  add_compile_definitions(PP_BROWSER_HEADLESS=1)
  message(STATUS "pp-browser: PP_BROWSER_HEADLESS=ON (node-only; no GUI deps)")
  if(PP_BROWSER_IS_MOBILE)
    message(FATAL_ERROR "PP_BROWSER_HEADLESS is for desktop pp-node builds only (not Android/iOS)")
  endif()
endif()

option(PP_BROWSER_MOBILE "Target is a mobile platform (Android or iOS)" "${PP_BROWSER_IS_MOBILE}")

if(PP_BROWSER_IS_MOBILE AND NOT PP_BROWSER_MOBILE)
  set(PP_BROWSER_MOBILE ON CACHE BOOL "Target is a mobile platform (Android or iOS)" FORCE)
endif()

if(PP_BROWSER_IS_MOBILE)
  set(PPBROWSER_ENABLE_DEBUGGER_DEFAULT OFF)
else()
  set(PPBROWSER_ENABLE_DEBUGGER_DEFAULT ON)
endif()

if(PP_BROWSER_IS_ANDROID)
  set(PP_BROWSER_APP_TARGET main)
  # Boost GDB pretty-printer inline asm uses @progbits (GNU syntax) which breaks
  # armeabi-v7a / clang; not needed on device.
  add_compile_definitions(
    BOOST_ALL_NO_EMBEDDED_GDB_SCRIPTS
    BOOST_OUTCOME_DISABLE_INLINE_GDB_PRETTY_PRINTERS
    BOOST_OUTCOME_SYSTEM_ERROR2_DISABLE_INLINE_GDB_PRETTY_PRINTERS
  )
else()
  set(PP_BROWSER_APP_TARGET pp-browser)
endif()

if(WIN32)
  set(PP_BROWSER_DESKTOP_OS WIN32)
elseif(APPLE)
  set(PP_BROWSER_DESKTOP_OS DARWIN)
elseif(UNIX)
  set(PP_BROWSER_DESKTOP_OS LINUX)
else()
  set(PP_BROWSER_DESKTOP_OS LINUX)
endif()

message(STATUS "pp-browser platform: android=${PP_BROWSER_IS_ANDROID} ios=${PP_BROWSER_IS_IOS} mobile=${PP_BROWSER_MOBILE} app_target=${PP_BROWSER_APP_TARGET} desktop_os=${PP_BROWSER_DESKTOP_OS}")
