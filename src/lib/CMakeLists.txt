include(PpBrowserBase)

# Lib peers (strict independence).
add_subdirectory(people)
# net is available in headless too (pp-node uses HTTP helpers).
add_subdirectory(net)

set(_pp_lib_peers
  pp_lib_people
  pp_lib_net
)

if(NOT PP_BROWSER_HEADLESS)
  add_subdirectory(messaging)
  add_subdirectory(calls)
  add_subdirectory(broadcast)
  add_subdirectory(pricing)
  add_subdirectory(content)
  add_subdirectory(ai)
  add_subdirectory(media)
  # Product shell (theme/catalogs). Host/overlays: foundation/platform/ui/.
  add_subdirectory(ui)
  list(APPEND _pp_lib_peers
    pp_lib_messaging
    pp_lib_calls
    pp_lib_broadcast
    pp_lib_pricing
    pp_lib_content
    pp_lib_ai
    pp_lib_ai_conversation
    pp_lib_ai_mcp
    pp_lib_ai_tools
    pp_lib_media
    pp_lib_ui
  )
endif()

# Aggregate for this build's lib peers (mirrors pp_foundation).
add_library(pp_lib INTERFACE)
target_link_libraries(pp_lib INTERFACE ${_pp_lib_peers})
unset(_pp_lib_peers)
