# AssetIO / IAssetLocator / DesktopAssetLocator live in platform_core so i18n
# and pp-node never need pp_foundation_platform (SDL / RmlUi). Desktop AssetIO
# is ifstream-only; mobile packaged reads use SDL (linked onto core below).
set(_pp_platform_core_sources
  DeploymentProfile.cpp
  PickedFileName.cpp
  Platform.cpp
  PlatformLogDefaults.cpp
  IPathProvider.cpp
  DesktopPathProvider.cpp
  desktop/PathHelpers.cpp
  ICredentialStore.cpp
  EnvCredentialStore.cpp
  ExecutablePath.cpp
  os/OsProcess_Posix.cpp
  os/OsThreadName_Posix.cpp
  os/OsLocaleTags.cpp
  os/KeychainStatus.cpp
  AssetIO.cpp
  IAssetLocator.cpp
  DesktopAssetLocator.cpp
)

set(_pp_platform_gui_sources
  AndroidAssetLocator.cpp
  AndroidPathProvider.cpp
  AppEventHooks.cpp
  CallSystemUi.cpp
  DesktopLocalNotifier.cpp
  desktop/LocalNotifierActivation.cpp
  ILocalNotifier.cpp
  IncomingLink.cpp
  IosAssetLocator.cpp
  IosPathProvider.cpp
  MobileWindowSizing.cpp
  NetworkConnectivity.cpp
  NetworkMonitor.cpp
  PlatformEarlyInit.cpp
  AiImagePrep.cpp
  ImageEncode.cpp
  ProfileIconImagePrep.cpp
  VideoPosterExtractor_Common.cpp
  PlatformLogSink.cpp
  PlatformOpenUrl.cpp
  PlatformHooks.cpp
  PlatformStartupHints.cpp
  PlatformUserHints.cpp
  PushWakeJni.cpp
  SdlAssetFileInterface.cpp
  WindowIcon.cpp
  AndroidLocalNotifier.cpp
  AndroidPushDeviceRegistrar.cpp
  AndroidSystemChrome.cpp
)

# The platform's TLS trust for curl is a base mechanism: foundation/tls (CR015).
if(PP_BROWSER_IS_IOS)
  list(APPEND _pp_platform_core_sources IosBackupExclusion_Ios.mm)
endif()

# Uninstall-surviving blob store (OsSecureBlobStore, behind common/SecureBlobStore.h): iOS Keychain; Android a
# Keystore-sealed no-backup file (PpSecureBlob.java, API 28+); else a stub.
list(APPEND _pp_platform_core_sources OsSecureBlobStore.cpp)
if(PP_BROWSER_IS_IOS)
  list(APPEND _pp_platform_core_sources os/OsSecureBlob_Ios.mm)
elseif(PP_BROWSER_IS_ANDROID)
  list(APPEND _pp_platform_core_sources os/OsSecureBlob_Android.cpp)
else()
  list(APPEND _pp_platform_core_sources os/OsSecureBlob_Default.cpp)
endif()

# Protected-data state + the PIN grace clock (#21): iOS UIKit / sysctl / mach; Android KeyguardManager /
# ACTION_USER_PRESENT / BOOT_COUNT / CLOCK_BOOTTIME; a stub elsewhere (macOS, Windows, Linux).
if(PP_BROWSER_IS_IOS)
  list(APPEND _pp_platform_core_sources os/OsProtectedData_Ios.mm)
elseif(PP_BROWSER_IS_ANDROID)
  list(APPEND _pp_platform_core_sources os/OsProtectedData_Android.cpp)
else()
  list(APPEND _pp_platform_core_sources os/OsProtectedData_Default.cpp)
endif()

# Whether the app is on screen now (PIN grace window: only the foreground extends it): iOS asks UIKit; Android
# MainActivity (onStart..onStop); else true.
if(PP_BROWSER_IS_IOS)
  list(APPEND _pp_platform_core_sources os/OsAppState_Ios.mm)
elseif(PP_BROWSER_IS_ANDROID)
  list(APPEND _pp_platform_core_sources os/OsAppState_Android.cpp)
else()
  list(APPEND _pp_platform_core_sources os/OsAppState_Default.cpp)
endif()

# Which face / finger sensor the biometric unlock switch names (at-rest A014): iOS LocalAuthentication; else None.
if(PP_BROWSER_IS_IOS)
  list(APPEND _pp_platform_core_sources os/OsBiometry_Ios.mm)
else()
  list(APPEND _pp_platform_core_sources os/OsBiometry_Default.cpp)
endif()

if(PP_BROWSER_DESKTOP_OS STREQUAL "WIN32")
  list(APPEND _pp_platform_core_sources
    os/OsThreadName_Win32.cpp
    os/OsProcess_Win32.cpp
    os/OsLocale_Win32.cpp
    os/OsExecutablePath_Win32.cpp
    os/OsDownloadMark_Win32.cpp
    os/OsProcessStats_Default.cpp
    os/OsThreadStackDump_Default.cpp
    desktop/PathProvider_Win32.cpp
  )
  list(APPEND _pp_platform_gui_sources
    desktop/LocalNotifier_Win32.cpp
    NetworkMonitor_Win32.cpp
    VideoPosterExtractor_Win32.cpp
    ImageThumbnail_Default.cpp
    PlatformShare_Default.cpp
    NativeFileDialog_Default.cpp
    CameraCapture_Default.cpp
    PlatformOpenFile.cpp
  )
elseif(PP_BROWSER_DESKTOP_OS STREQUAL "DARWIN")
  list(APPEND _pp_platform_core_sources
    os/OsExecutablePath_Darwin.cpp
    os/OsLocale_Darwin.mm
    os/OsProcessStats_Default.cpp
    os/OsThreadStackDump_Darwin.cpp
    desktop/PathProvider_Darwin.cpp
  )
  if(PP_BROWSER_IS_IOS)
    list(APPEND _pp_platform_core_sources os/OsDownloadMark_Default.cpp)
  else()
    list(APPEND _pp_platform_core_sources os/OsDownloadMark_Darwin.cpp)
  endif()
  list(APPEND _pp_platform_gui_sources
    desktop/LocalNotifier_Darwin.mm
    desktop/WindowChrome_Darwin.mm
    NetworkMonitor_Darwin.mm
    ImageThumbnail_Darwin.mm
  )
  if(NOT PP_BROWSER_IS_IOS)
    list(APPEND _pp_platform_gui_sources VideoPosterExtractor_Darwin.mm PlatformShare_Darwin.mm
         NativeFileDialog_Default.cpp CameraCapture_Default.cpp PlatformOpenFile.cpp)
  else()
    # SDL3 has no iOS file dialog (its dummy backend answers "nothing picked" at once).
    list(APPEND _pp_platform_gui_sources VideoPosterExtractor_Default.cpp PlatformShare_Ios.mm
         NativeFileDialog_Ios.mm CameraCapture_Ios.mm PlatformOpenFile_Ios.mm)
  endif()
else()
  list(APPEND _pp_platform_core_sources
    os/OsExecutablePath_Linux.cpp
    os/OsDownloadMark_Default.cpp
    os/OsProcessStats_Linux.cpp
    os/OsThreadStackDump_Linux.cpp
    desktop/PathProvider_Linux.cpp
  )
  list(APPEND _pp_platform_gui_sources
    desktop/LocalNotifier_Linux.cpp
    ImageThumbnail_Default.cpp
    VideoPosterExtractor_Default.cpp
    PlatformShare_Default.cpp
    NativeFileDialog_Default.cpp
  )
  if(PP_BROWSER_IS_ANDROID)
    list(APPEND _pp_platform_core_sources os/OsLocale_Android.cpp)
  else()
    list(APPEND _pp_platform_core_sources os/OsLocale_Posix.cpp)
  endif()
  if(NOT PP_BROWSER_IS_ANDROID)
    list(APPEND _pp_platform_gui_sources NetworkMonitor_Linux.cpp CameraCapture_Default.cpp PlatformOpenFile.cpp)
  else()
    # A file:// address is refused by Android 7+ (FileUriExposedException): a FileProvider address instead.
    list(APPEND _pp_platform_gui_sources CameraCapture_Android.cpp PlatformOpenFile_Android.cpp)
  endif()
endif()

# Headless-safe platform surface (paths, assets, env credentials, OS file/time/process).
# No RmlUi / desktop SDL — used by pp-node, i18n, and pp_foundation_data.
pp_browser_add_foundation_library(pp_foundation_platform_core
  SOURCES
    ${_pp_platform_core_sources}
  PUBLIC_LIBS
    CURL::libcurl
    pp_foundation_product
)

# Mobile packaged AssetIO uses SDL_IOFromFile; pp-node is desktop-only so this
# does not widen the headless link surface.
if(PP_BROWSER_IS_MOBILE)
  if(NOT PP_BROWSER_SDL3_TARGET)
    message(FATAL_ERROR "pp_foundation_platform_core: PP_BROWSER_SDL3_TARGET unset on mobile")
  endif()
  target_link_libraries(pp_foundation_platform_core PUBLIC ${PP_BROWSER_SDL3_TARGET})
endif()

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

# Compile-time backend origins (scheme://host, no path); PP_BROWSER_ORIGIN overrides them at runtime.
# Empty by default: backend-dependent features stay disabled until a prod origin is supplied;
# an empty QA origin hides the QA backend option.
set(PP_API_URL_PROD "" CACHE STRING "Prod backend origin compiled into the client")
set(PP_API_URL_QA "" CACHE STRING "QA backend origin compiled into the client")
if(NOT PP_API_URL_PROD STREQUAL "")
  target_compile_definitions(pp_foundation_platform_core PRIVATE
    PP_API_URL_PROD="${PP_API_URL_PROD}")
endif()
if(NOT PP_API_URL_QA STREQUAL "")
  target_compile_definitions(pp_foundation_platform_core PRIVATE
    PP_API_URL_QA="${PP_API_URL_QA}")
endif()

if(PP_BROWSER_HEADLESS)
  return()
endif()

# OBJCXX is enabled in the top-level project() for desktop Darwin (required so
# CMAKE_OBJCXX_COMPILE_OBJECT is set at generate time).

# Low-level UI host (window / RmlUi Backend / overlays) — same foundation band.
# lib/ui is product shell only (theme/catalogs). See PLATFORM_CODE.md.
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
    ui::core
    ${PP_BROWSER_SDL3_TARGET}
    ${PP_BROWSER_SDL3_IMAGE_TARGET})

target_compile_definitions(pp_foundation_platform PUBLIC UI_SDL_VERSION_MAJOR=3)
if(UI_BACKEND_SIMULATE_TOUCH)
  target_compile_definitions(pp_foundation_platform PUBLIC UI_BACKEND_SIMULATE_TOUCH)
endif()
if(PP_BROWSER_IS_IOS)
  # OpenGLES is deprecated on iOS 12+; we still use ES3 via SDL until Metal.
  target_compile_definitions(pp_foundation_platform PRIVATE GLES_SILENCE_DEPRECATION=1)
endif()

# Unprefixed includes for host/overlays (CallVideoTileRenderer.h, etc.).
# Prefixed API: foundation/platform/ui/RmlUi_Backend.h, DesktopWindowChrome.h, …
# Include_GL3.h (glad) lives under pp-cpp-ui src/render/gl.
target_include_directories(pp_foundation_platform PUBLIC
  ${PP_LIB_UI_INCLUDE}
  ${PP_LIB_UI_ROOT}/src/render/gl
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

# iOS: curl uses BoringSSL (the SecTrust verify callback is foundation/tls's now, B8); the platform libs keep
# the same links for their own Keychain / curl users.
if(PP_BROWSER_IS_IOS)
  target_link_libraries(pp_foundation_platform_core PUBLIC
    CURL::libcurl
    OpenSSL::SSL
    OpenSSL::Crypto
    "-framework Security"
    "-framework CoreFoundation"
    "-framework Foundation"
    "-framework UIKit"
    "-framework LocalAuthentication")
  target_link_libraries(pp_foundation_platform PUBLIC
    CURL::libcurl
    OpenSSL::SSL
    OpenSSL::Crypto
    "-framework Security"
    "-framework CoreFoundation")
endif()

# macOS: OsLocale_Darwin.mm (platform_core, also linked by pp-node) needs Foundation; iOS adds it above.
if(PP_BROWSER_DESKTOP_OS STREQUAL "DARWIN" AND NOT PP_BROWSER_IS_IOS)
  target_link_libraries(pp_foundation_platform_core PUBLIC "-framework Foundation")
endif()

# Android: ApplyPlatformCurlSsl sets CURLOPT_CAPATH via curl.
if(PP_BROWSER_IS_ANDROID)
  target_sources(pp_foundation_platform PRIVATE NetworkConnectivity_Android.cpp NetworkMonitor_Android.cpp
                 StayConnectedService_Android.cpp
                 AndroidCallSystemUi.cpp SecureScreen_Android.cpp
                 OngoingCallService_Android.cpp)
  target_link_libraries(pp_foundation_platform PUBLIC CURL::libcurl)
else()
  target_sources(pp_foundation_platform PRIVATE StayConnectedService_Default.cpp OngoingCallService_Default.cpp)
  if(WIN32)
    target_sources(pp_foundation_platform PRIVATE SecureScreen_Win32.cpp)
  elseif(APPLE AND NOT PP_BROWSER_IS_IOS)
    target_sources(pp_foundation_platform PRIVATE SecureScreen_Darwin.mm)
  elseif(NOT PP_BROWSER_IS_IOS)
    target_sources(pp_foundation_platform PRIVATE SecureScreen_Default.cpp)
  endif()
endif()

if(PP_BROWSER_IS_IOS)
  target_sources(pp_foundation_platform PRIVATE
    NetworkConnectivity_Ios.mm
    IosPushDeviceRegistrar.mm
    IosLocalNotifier.mm
    IosApnsAppDelegate.mm
    SecureScreen_Ios.mm)
  target_link_libraries(pp_foundation_platform PUBLIC
    "-framework UserNotifications"
    "-framework UIKit"
    "-framework PhotosUI"
    "-framework QuickLook"
    "-framework UniformTypeIdentifiers")
endif()

# On-screen keyboard height: only iOS needs it (its safe area leaves the keyboard out).
if(PP_BROWSER_IS_IOS)
  target_sources(pp_foundation_platform PRIVATE ScreenKeyboard_Ios.mm)
else()
  target_sources(pp_foundation_platform PRIVATE ScreenKeyboard_Default.cpp)
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
  target_link_libraries(pp_foundation_platform PRIVATE mfplat mfreadwrite mfuuid iphlpapi ws2_32)
endif()

# NetworkMonitor_Darwin: NWPathMonitor (macOS and iOS). ImageThumbnail_Darwin: ImageIO downsampled decode.
if(PP_BROWSER_DESKTOP_OS STREQUAL "DARWIN")
  target_link_libraries(pp_foundation_platform PUBLIC "-framework Network" "-framework ImageIO"
                        "-framework CoreGraphics" "-framework CoreFoundation")
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
