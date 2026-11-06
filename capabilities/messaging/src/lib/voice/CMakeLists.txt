# Voice messages (CR023 step 2): recording and playing a clip as Ogg Opus, the touch-and-hold gesture, and the
# audio session they share with calls, on the media capability's device leases.
pp_browser_add_lib_library(pp_lib_voice
  SOURCES
    AudioFilePlayer.cpp
    OggOpus.cpp
    VoiceAudioSession.cpp
    VoiceDeviceBlock.cpp
    VoiceFile.cpp
    VoicePlayer.cpp
    VoiceRecorder.cpp
  PUBLIC_LIBS
    pp_lib_media)

# Sound-file attachments (MP3 ...) play through the system's player: AVAudioPlayer on Apple, MediaPlayer on Android;
# elsewhere they open in the system's app.
if(APPLE)
  target_sources(pp_lib_voice PRIVATE AudioFilePlayer_Darwin.mm)
  target_link_libraries(pp_lib_voice PRIVATE "-framework AVFoundation" "-framework Foundation")
elseif(ANDROID)
  target_sources(pp_lib_voice PRIVATE AudioFilePlayer_Android.cpp)
else()
  target_sources(pp_lib_voice PRIVATE AudioFilePlayer_Default.cpp)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID)
  target_sources(pp_lib_voice PRIVATE VoiceSupport_Linux.cpp)
else()
  target_sources(pp_lib_voice PRIVATE VoiceSupport_Default.cpp)
endif()

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
