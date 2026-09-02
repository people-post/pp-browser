pp_browser_add_base_library(pp_base_media
  SOURCES
    CallMediaAdaptation.cpp
    CallMediaEngine.cpp
    CallRingtone.cpp
    VideoCodecFactory.cpp
    VideoCodecUnavailable.cpp
    VideoYuv.cpp
  PUBLIC_LIBS
    pp_foundation_error
    pp_foundation_platform
    ${PP_BROWSER_SDL3_TARGET}
    Opus::opus)

# Domain OS backends — source-select; do not compile MediaCodec TUs on Linux, etc.
if(WIN32)
  target_sources(pp_base_media PRIVATE
    VideoCodec_Win32.cpp
    CallAudioSession_Default.cpp
    CameraCaptureOrientation_Default.cpp)
  target_link_libraries(pp_base_media PRIVATE mfplat mf mfuuid)
elseif(APPLE)
  target_sources(pp_base_media PRIVATE VideoCodec_Darwin.cpp)
  target_link_libraries(pp_base_media PRIVATE
    "-framework VideoToolbox"
    "-framework CoreMedia"
    "-framework CoreVideo"
    "-framework CoreFoundation")
  if(PP_BROWSER_IS_IOS)
    target_sources(pp_base_media PRIVATE
      CallAudioSession_Ios.mm
      CameraCaptureOrientation_Ios.mm)
    target_link_libraries(pp_base_media PRIVATE "-framework AVFoundation" "-framework UIKit")
  else()
    target_sources(pp_base_media PRIVATE
      CallAudioSession_Default.cpp
      CameraCaptureOrientation_Default.cpp)
  endif()
elseif(ANDROID)
  target_sources(pp_base_media PRIVATE
    VideoCodec_Android.cpp
    CallAudioSession_Android.cpp
    CameraCaptureOrientation_Android.cpp)
  target_link_libraries(pp_base_media PRIVATE mediandk camera2ndk)
else()
  target_sources(pp_base_media PRIVATE
    VideoCodec_Linux.cpp
    CallAudioSession_Default.cpp
    CameraCaptureOrientation_Default.cpp)
  # Linux VA-API (libva + DRM render node) — soft dep; stub builds without it.
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(PP_LIBVA QUIET libva libva-drm)
  endif()
  if(PP_LIBVA_FOUND)
    target_compile_definitions(pp_base_media PRIVATE PP_BROWSER_HAS_LIBVA=1)
    target_include_directories(pp_base_media PRIVATE ${PP_LIBVA_INCLUDE_DIRS})
    target_link_libraries(pp_base_media PRIVATE ${PP_LIBVA_LIBRARIES})
    message(STATUS "pp-browser: Linux H264 — libva + libva-drm (VA-API)")
  else()
    message(WARNING
      "Linux H264 encode/decode needs libva development packages (best-effort VA-API).\n"
      "  Without them VideoCodec_Linux falls back to the unavailable stub (voice still works).\n"
      "  Debian/Ubuntu: sudo apt install libva-dev\n"
      "  Runtime also needs a VA driver (e.g. intel-media-va-driver / mesa-va-drivers).")
  endif()
endif()

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
