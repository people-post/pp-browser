# The media capability (projects/capability-repos CR023 step 2): the devices, hardware codecs and media engine every
# realtime feature runs on — calls, broadcast, voice messages, the QR scanner ([L016]: no control thread waits on
# them). Below all of those; on the base only. Its src/ mirrors the tree it came from ("lib/media/…").
pp_browser_capability(media)

# Its common layer (headers only): what the engine reports and what it is told to send at, for the features that
# turn them into their own terms (calls: call quality, adaptation).
add_library(pp_media_common INTERFACE)
target_include_directories(pp_media_common INTERFACE ${PP_BROWSER_CAPABILITY_SRC})

# SDL3 devices and the platform codecs: GUI configures only.
if(NOT PP_BROWSER_HEADLESS)
  add_subdirectory(src/lib/media)
endif()
