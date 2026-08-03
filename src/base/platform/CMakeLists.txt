set(_pp_platform_core_sources
  Platform.cpp
  PlatformLogDefaults.cpp
  IPathProvider.cpp
  DesktopPathProvider.cpp
  desktop/PathHelpers.cpp
  ICredentialStore.cpp
  EnvCredentialStore.cpp
  ExecutablePath.cpp
  ThreadRuntime.cpp
  os/OsFile_Posix.cpp
  os/OsTime_Posix.cpp
  os/OsProcess_Posix.cpp
)

set(_pp_platform_gui_sources
  AndroidAssetLocator.cpp
  AndroidPathProvider.cpp
  AppEventHooks.cpp
  AppLifecycle.cpp
  AssetIO.cpp
  BackgroundSyncScheduler.cpp
  BrowserThread.cpp
  DesktopAssetLocator.cpp
  DesktopLocalNotifier.cpp
  DesktopWindowChrome.cpp
  desktop/LocalNotifierActivation.cpp
  IAssetLocator.cpp
  ILocalNotifier.cpp
  IosAssetLocator.cpp
  IosPathProvider.cpp
  IPushDeviceRegistrar.cpp
  MobileWindowSizing.cpp
  NetworkConnectivity.cpp
  PlatformEarlyInit.cpp
  PlatformLogSink.cpp
  PlatformNavigation.cpp
  PlatformOpenUrl.cpp
  PlatformServices.cpp
  PlatformStartupHints.cpp
  PlatformUserHints.cpp
  PushWakeJni.cpp
  SdlAppEvents.cpp
  SdlAssetFileInterface.cpp
  WindowIcon.cpp
  AndroidLocalNotifier.cpp
  AndroidPushDeviceRegistrar.cpp
  AndroidSystemChrome.cpp
  os/OsTlsCaPath_Android.cpp
  os/OsTlsCaPath_Default.cpp
  os/OsTlsPlatformCurl_Android.cpp
  os/OsTlsPlatformCurl_Default.cpp
  os/OsTlsPlatformCurl_Ios.cpp
)

if(PP_BROWSER_DESKTOP_OS STREQUAL "WIN32")
  list(APPEND _pp_platform_core_sources
    os/OsFile_Win32.cpp
    os/OsTime_Win32.cpp
    os/OsProcess_Win32.cpp
    os/OsExecutablePath_Win32.cpp
    desktop/PathProvider_Win32.cpp
  )
  list(APPEND _pp_platform_gui_sources
    desktop/LocalNotifier_Win32.cpp
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
else()
  list(APPEND _pp_platform_core_sources
    os/OsExecutablePath_Linux.cpp
    desktop/PathProvider_Linux.cpp
  )
  list(APPEND _pp_platform_gui_sources
    desktop/LocalNotifier_Linux.cpp
  )
endif()

# Headless-safe platform surface (paths, env credentials, OS file/time/process).
# No SDL / RmlUi — used by pp-node and by pp_base_data.
pp_browser_add_base_library(pp_base_platform_core
  SOURCES
    ${_pp_platform_core_sources}
)

target_compile_definitions(pp_base_platform_core PUBLIC
  "PP_BROWSER_RELEASE_VERSION=\"${PP_BROWSER_RELEASE_VERSION}\""
  "PP_BROWSER_VERSION=\"${PP_BROWSER_VERSION}\"")

# OBJCXX is enabled in the top-level project() for desktop Darwin (required so
# CMAKE_OBJCXX_COMPILE_OBJECT is set at generate time).

pp_browser_add_base_library(pp_base_platform
  SOURCES
    ${_pp_platform_gui_sources}
  PUBLIC_LIBS
    pp_base_platform_core
    pp_rmlui_backend
    RmlUi::Core
    ${PP_BROWSER_SDL3_TARGET}
    ${PP_BROWSER_SDL3_IMAGE_TARGET})

# iOS: curl uses BoringSSL; OsTlsPlatformCurl_Ios installs a SecTrust verify callback.
if(PP_BROWSER_IS_IOS)
  target_link_libraries(pp_base_platform PUBLIC
    CURL::libcurl
    OpenSSL::SSL
    OpenSSL::Crypto
    "-framework Security"
    "-framework CoreFoundation")
endif()

# Android: ApplyPlatformCurlSsl sets CURLOPT_CAPATH via curl.
if(PP_BROWSER_IS_ANDROID)
  target_sources(pp_base_platform PRIVATE NetworkConnectivity_Android.cpp)
  target_link_libraries(pp_base_platform PUBLIC CURL::libcurl)
endif()

if(PP_BROWSER_IS_IOS)
  target_sources(pp_base_platform PRIVATE NetworkConnectivity_Ios.mm)
endif()

# Desktop macOS: UNUserNotificationCenter for local banners.
if(PP_BROWSER_DESKTOP_OS STREQUAL "DARWIN" AND NOT PP_BROWSER_IS_IOS)
  target_link_libraries(pp_base_platform PUBLIC
    "-framework AppKit"
    "-framework QuartzCore"
    "-framework UserNotifications"
    "-framework CoreServices"
    "-framework Foundation")
endif()

# Desktop Linux: Freedesktop Notifications via libdbus-1; X11 raise uses libX11.
if(PP_BROWSER_DESKTOP_OS STREQUAL "LINUX" AND NOT PP_BROWSER_IS_ANDROID)
  find_package(PkgConfig REQUIRED)
  find_package(X11 REQUIRED)
  pkg_check_modules(PP_DBUS REQUIRED IMPORTED_TARGET dbus-1)
  target_link_libraries(pp_base_platform PUBLIC PkgConfig::PP_DBUS X11::X11)
endif()

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
