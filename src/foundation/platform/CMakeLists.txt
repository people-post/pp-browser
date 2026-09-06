set(_pp_platform_core_sources
  DeploymentProfile.cpp
  Platform.cpp
  PlatformLogDefaults.cpp
  IPathProvider.cpp
  DesktopPathProvider.cpp
  desktop/PathHelpers.cpp
  ICredentialStore.cpp
  EnvCredentialStore.cpp
  ExecutablePath.cpp
  os/OsFile_Posix.cpp
  os/OsProcess_Posix.cpp
  CurlSsl.cpp
)

set(_pp_platform_gui_sources
  AndroidAssetLocator.cpp
  AndroidPathProvider.cpp
  AppEventHooks.cpp
  AssetIO.cpp
  DesktopAssetLocator.cpp
  DesktopLocalNotifier.cpp
  desktop/LocalNotifierActivation.cpp
  IAssetLocator.cpp
  ILocalNotifier.cpp
  IosAssetLocator.cpp
  IosPathProvider.cpp
  IPushDeviceRegistrar.cpp
  MobileWindowSizing.cpp
  NativeFileDialog.cpp
  NetworkConnectivity.cpp
  PlatformEarlyInit.cpp
  ProfileIconImagePrep.cpp
  VideoPosterExtractor_Common.cpp
  PlatformLogSink.cpp
  PlatformOpenUrl.cpp
  PlatformOpenFile.cpp
  PlatformHooks.cpp
  PlatformStartupHints.cpp
  PlatformUserHints.cpp
  PushWakeJni.cpp
  SdlAssetFileInterface.cpp
  WindowIcon.cpp
  AndroidLocalNotifier.cpp
  AndroidPushDeviceRegistrar.cpp
  AndroidSystemChrome.cpp
  os/OsTlsCaPath_Android.cpp
  os/OsTlsCaPath_Default.cpp
)

# CurlSsl + ApplyPlatformCurlSsl live in platform_core so ai/net can use TLS
# defaults without linking the SDL/Rml GUI platform lib.
if(PP_BROWSER_IS_ANDROID)
  list(APPEND _pp_platform_core_sources
    os/OsTlsPlatformCurl_Android.cpp
    os/OsTlsCaPath_Android.cpp
  )
elseif(PP_BROWSER_IS_IOS)
  list(APPEND _pp_platform_core_sources os/OsTlsPlatformCurl_Ios.cpp)
else()
  list(APPEND _pp_platform_core_sources os/OsTlsPlatformCurl_Default.cpp)
endif()

if(PP_BROWSER_DESKTOP_OS STREQUAL "WIN32")
  list(APPEND _pp_platform_core_sources
    os/OsFile_Win32.cpp
    os/OsProcess_Win32.cpp
    os/OsExecutablePath_Win32.cpp
    desktop/PathProvider_Win32.cpp
  )
  list(APPEND _pp_platform_gui_sources
    desktop/LocalNotifier_Win32.cpp
    VideoPosterExtractor_Win32.cpp
  )
elseif(PP_BROWSER_DESKTOP_OS STREQUAL "DARWIN")
  list(APPEND _pp_platform_core_sources
    os/OsExecutablePath_Darwin.cpp
    desktop/PathProvider_Darwin.cpp
  )
  list(APPEND _pp_platform_gui_sources
    desktop/LocalNotifier_Darwin.mm
    desktop/WindowChrome_Darwin.mm
  )
  if(NOT PP_BROWSER_IS_IOS)
    list(APPEND _pp_platform_gui_sources VideoPosterExtractor_Darwin.mm)
  else()
    list(APPEND _pp_platform_gui_sources VideoPosterExtractor_Default.cpp)
  endif()
else()
  list(APPEND _pp_platform_core_sources
    os/OsExecutablePath_Linux.cpp
    desktop/PathProvider_Linux.cpp
  )
  list(APPEND _pp_platform_gui_sources
    desktop/LocalNotifier_Linux.cpp
    VideoPosterExtractor_Default.cpp
  )
endif()

# Headless pp-node still needs AssetIO / IAssetLocator for i18n (via pp_foundation_error).
# Desktop AssetIO is ifstream-only; no SDL. CurlSsl TLS defaults already live in core.
if(PP_BROWSER_HEADLESS)
  list(APPEND _pp_platform_core_sources
    AssetIO.cpp
    IAssetLocator.cpp
    DesktopAssetLocator.cpp
  )
endif()

# Headless-safe platform surface (paths, env credentials, OS file/time/process).
# No SDL / RmlUi — used by pp-node and by pp_foundation_data.
pp_browser_add_foundation_library(pp_foundation_platform_core
  SOURCES
    ${_pp_platform_core_sources}
  PUBLIC_LIBS
    CURL::libcurl
)

# DesktopPathProvider::BundleAssetsDir() reads these; they must be set on the
# target that compiles it, not only on the app target (which links this lib).
# Without them the packaged branch is compiled out and assets resolve relative
# to the working directory, so a launched PP.app finds no assets.
if(PP_BROWSER_PACKAGED_BUILD OR PP_BROWSER_IS_MOBILE)
  target_compile_definitions(pp_foundation_platform_core PRIVATE PP_BROWSER_PACKAGED_BUILD=1)
else()
  target_compile_definitions(pp_foundation_platform_core PRIVATE
    PP_BROWSER_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")
endif()

if(PP_BROWSER_HEADLESS)
  return()
endif()

# OBJCXX is enabled in the top-level project() for desktop Darwin (required so
# CMAKE_OBJCXX_COMPILE_OBJECT is set at generate time).

# Low-level UI host (window / RmlUi Backend / overlays) — same foundation band.
# domain/ui is product shell only (theme/catalogs). See PLATFORM_CODE.md.
set(_pp_platform_ui_sources
  ui/DesktopWindowChrome.cpp
  ui/PlatformNavigation.cpp
  ui/SdlAppEvents.cpp
  ui/gl/MobileGlLifecycle.cpp
  ui/renderer/TextLoupeRenderer.cpp
  ui/renderer/CallVideoTileRenderer.cpp
  ui/renderer/ElementCallVideoTile.cpp
  ui/host/BrowserHost.cpp
  ui/host/TouchSimOverlay.cpp
)

include(pp_lib_rmlui)

pp_browser_add_foundation_library(pp_foundation_platform
  SOURCES
    ${_pp_platform_gui_sources}
    ${_pp_platform_ui_sources}
  PUBLIC_LIBS
    pp_foundation_platform_core
    pp_foundation_runtime
    pp_ui
    RmlUi::Core
    ${PP_BROWSER_SDL3_TARGET}
    ${PP_BROWSER_SDL3_IMAGE_TARGET})

target_compile_definitions(pp_foundation_platform PUBLIC RMLUI_SDL_VERSION_MAJOR=3)
if(RMLUI_BACKEND_SIMULATE_TOUCH)
  target_compile_definitions(pp_foundation_platform PUBLIC RMLUI_BACKEND_SIMULATE_TOUCH)
endif()
if(PP_BROWSER_IS_IOS)
  # OpenGLES is deprecated on iOS 12+; we still use ES3 via SDL until Metal.
  target_compile_definitions(pp_foundation_platform PRIVATE GLES_SILENCE_DEPRECATION=1)
endif()

# Unprefixed includes for host/overlays (CallVideoTileRenderer.h, etc.).
# Prefixed API: foundation/platform/ui/RmlUi_Backend.h, DesktopWindowChrome.h, …
target_include_directories(pp_foundation_platform PUBLIC
  ${PP_LIB_RMLUI_INCLUDE}
  ${CMAKE_CURRENT_SOURCE_DIR}/ui
  ${CMAKE_CURRENT_SOURCE_DIR}/ui/gl
  ${CMAKE_CURRENT_SOURCE_DIR}/ui/renderer
  ${CMAKE_CURRENT_SOURCE_DIR}/ui/host
)

pp_browser_link_rmlui_svg_deps(pp_foundation_platform)
if(UNIX AND NOT APPLE)
  target_link_libraries(pp_foundation_platform PUBLIC ${CMAKE_DL_LIBS})
endif()
if(ANDROID)
  target_link_libraries(pp_foundation_platform PUBLIC log GLESv3 EGL)
endif()

# iOS: curl uses BoringSSL; OsTlsPlatformCurl_Ios installs a SecTrust verify callback.
if(PP_BROWSER_IS_IOS)
  target_link_libraries(pp_foundation_platform PUBLIC
    CURL::libcurl
    OpenSSL::SSL
    OpenSSL::Crypto
    "-framework Security"
    "-framework CoreFoundation")
endif()

# Android: ApplyPlatformCurlSsl sets CURLOPT_CAPATH via curl.
if(PP_BROWSER_IS_ANDROID)
  target_sources(pp_foundation_platform PRIVATE NetworkConnectivity_Android.cpp)
  target_link_libraries(pp_foundation_platform PUBLIC CURL::libcurl)
endif()

if(PP_BROWSER_IS_IOS)
  target_sources(pp_foundation_platform PRIVATE NetworkConnectivity_Ios.mm)
endif()

# Desktop macOS: UNUserNotificationCenter for local banners.
if(PP_BROWSER_DESKTOP_OS STREQUAL "DARWIN" AND NOT PP_BROWSER_IS_IOS)
  target_link_libraries(pp_foundation_platform PUBLIC
    "-framework AppKit"
    "-framework QuartzCore"
    "-framework UserNotifications"
    "-framework CoreServices"
    "-framework Foundation"
    "-framework AVFoundation"
    "-framework CoreMedia"
    "-framework CoreVideo"
    "-framework CoreGraphics")
endif()

if(PP_BROWSER_DESKTOP_OS STREQUAL "WIN32")
  target_link_libraries(pp_foundation_platform PRIVATE mfplat mfreadwrite mfuuid)
endif()

# Desktop Linux: Freedesktop Notifications via libdbus-1; X11 raise uses libX11.
if(PP_BROWSER_DESKTOP_OS STREQUAL "LINUX" AND NOT PP_BROWSER_IS_ANDROID)
  find_package(PkgConfig REQUIRED)
  find_package(X11 REQUIRED)
  pkg_check_modules(PP_DBUS REQUIRED IMPORTED_TARGET dbus-1)
  target_link_libraries(pp_foundation_platform PUBLIC PkgConfig::PP_DBUS X11::X11)
endif()

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
