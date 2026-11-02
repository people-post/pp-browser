# The call's sounds: the incoming ringtone and the caller's ringback, on the media capability's speaker lease and
# audio session (a ringback holds the session while it plays: AudioSession tone holds).
pp_browser_add_lib_library(pp_lib_call_audio
  SOURCES
    CallRingtone.cpp
    Ringback.cpp
  PUBLIC_LIBS
    pp_lib_media)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
