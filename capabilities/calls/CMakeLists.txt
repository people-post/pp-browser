# The calls capability (projects/capability-repos CR003, CR020, CR022): 1:1 audio / video calls — the call vocabulary
# and stores, the media engine and devices (lib/media, on SDL3 and Opus), the media legs on the mesh (call_media) and
# the session orchestration (feature/calls). Delivery carries its signaling; it never uses messaging. Its src/ mirrors
# the tree it came from ("lib/calls/…", "feature/calls/…"). Its targets link only what they name.
pp_browser_capability(calls)
include(PpBrowserFeature)

# Its common layer: the media leg's health and wire names, shared with broadcast and the app.
add_library(pp_calls_common STATIC
  src/common/media/CallMediaHealth.cpp)
target_include_directories(pp_calls_common PUBLIC ${PP_BROWSER_CAPABILITY_SRC})
target_link_libraries(pp_calls_common PUBLIC pp_pbr_common)
pp_browser_apply_warnings(pp_calls_common)

# Headless too: pp-call-probe links the media legs.
add_subdirectory(src/domain/call_media)

if(NOT PP_BROWSER_HEADLESS)
  add_subdirectory(src/lib/calls)
  add_subdirectory(src/lib/media)
  add_subdirectory(src/feature/calls)
endif()
