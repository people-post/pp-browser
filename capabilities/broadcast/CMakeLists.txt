# The broadcast capability (projects/capability-repos CR003, CR022): live broadcast — join tickets, viewer ladders,
# relay media, the RPC codec and the signed peer-announce feed, and the viewer workflow. A sibling of calls on the
# same neutral layers; calls never names it. Its src/ mirrors the tree it came from ("lib/broadcast/…"). Its targets
# link only what they name.
pp_browser_capability(broadcast)
include(PpBrowserFeature)

if(NOT PP_BROWSER_HEADLESS)
  add_subdirectory(src/lib/broadcast)
  add_subdirectory(src/feature/broadcast)
endif()
