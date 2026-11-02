# Voice messages (CR023 step 2): recording and playing a clip as Ogg Opus, the touch-and-hold gesture, and the
# audio session they share with calls, on the media capability's device leases.
pp_browser_add_lib_library(pp_lib_voice
  SOURCES
    OggOpus.cpp
    VoiceAudioSession.cpp
    VoiceDeviceBlock.cpp
    VoiceFile.cpp
    VoicePlayer.cpp
    VoiceRecorder.cpp
  PUBLIC_LIBS
    pp_lib_media)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID)
  target_sources(pp_lib_voice PRIVATE VoiceSupport_Linux.cpp)
else()
  target_sources(pp_lib_voice PRIVATE VoiceSupport_Default.cpp)
endif()

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
