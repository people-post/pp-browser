set(_pp_platform_common_sources
  AndroidAssetLocator.cpp
  AndroidPathProvider.cpp
  AppEventHooks.cpp
  AppLifecycle.cpp
  AssetIO.cpp
  BackgroundSyncScheduler.cpp
  BrowserThread.cpp
  DesktopAssetLocator.cpp
  DesktopLocalNotifier.cpp
  DesktopPathProvider.cpp
  desktop/PathHelpers.cpp
  EnvCredentialStore.cpp
  ExecutablePath.cpp
  IAssetLocator.cpp
  ICredentialStore.cpp
  ILocalNotifier.cpp
  IosAssetLocator.cpp
  IosPathProvider.cpp
  IPathProvider.cpp
  IPushDeviceRegistrar.cpp
  MobileWindowSizing.cpp
  PlatformLogDefaults.cpp
  PlatformStartupHints.cpp
  AndroidLocalNotifier.cpp
  AndroidPushDeviceRegistrar.cpp
  Platform.cpp
  PlatformEarlyInit.cpp
  PlatformLogSink.cpp
  PlatformNavigation.cpp
  PlatformServices.cpp
  PushWakeJni.cpp
  SdlAppEvents.cpp
  SdlAssetFileInterface.cpp
  WindowIcon.cpp
)

set(_pp_platform_os_sources
  os/OsFile_Posix.cpp
  os/OsTime_Posix.cpp
  os/OsProcess_Posix.cpp
)

if(PP_BROWSER_DESKTOP_OS STREQUAL "WIN32")
  list(APPEND _pp_platform_os_sources
    os/OsFile_Win32.cpp
    os/OsTime_Win32.cpp
    os/OsProcess_Win32.cpp
    os/OsExecutablePath_Win32.cpp
    desktop/PathProvider_Win32.cpp
    desktop/LocalNotifier_Win32.cpp
  )
elseif(PP_BROWSER_DESKTOP_OS STREQUAL "DARWIN")
  list(APPEND _pp_platform_os_sources
    os/OsExecutablePath_Darwin.cpp
    desktop/PathProvider_Darwin.cpp
    desktop/LocalNotifier_Darwin.cpp
  )
else()
  list(APPEND _pp_platform_os_sources
    os/OsExecutablePath_Linux.cpp
    desktop/PathProvider_Linux.cpp
    desktop/LocalNotifier_Linux.cpp
  )
endif()

pp_browser_add_base_library(pp_base_platform
  SOURCES
    ${_pp_platform_common_sources}
    ${_pp_platform_os_sources}
  PUBLIC_LIBS
    pp_rmlui_backend
    RmlUi::Core
    ${PP_BROWSER_SDL3_TARGET}
    ${PP_BROWSER_SDL3_IMAGE_TARGET})

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
