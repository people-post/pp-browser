pp_browser_add_base_library(pp_base_media
  SOURCES
    CallMediaEngine.cpp
  PUBLIC_LIBS
    pp_base_error
    ${PP_BROWSER_SDL3_TARGET}
    Opus::opus
  PRIVATE_LIBS
    ${PP_BROWSER_DATACHANNEL_TARGET})

if(PP_BROWSER_BUILD_TESTS)
  # Media engine unit tests need devices; keep out of default CI for now.
endif()
