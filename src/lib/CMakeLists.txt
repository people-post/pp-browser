include(PpBrowserBase)

# Lib peers (strict independence). people, net, social and delivery are capability folders (capabilities/, CR022), added first.

set(_pp_lib_peers
  pp_lib_people
  pp_lib_net
)

if(NOT PP_BROWSER_HEADLESS)
  add_subdirectory(messaging)
  add_subdirectory(calls)
  add_subdirectory(broadcast)
  add_subdirectory(pricing)
  add_subdirectory(ai)
  add_subdirectory(media)
  # Product shell (theme/catalogs). Host/overlays: foundation/platform/ui/.
  add_subdirectory(ui)
  list(APPEND _pp_lib_peers
    pp_lib_people_cards
    pp_lib_delivery
    pp_lib_messaging
    pp_lib_calls
    pp_lib_broadcast
    pp_lib_pricing
    pp_lib_content
    pp_lib_posts
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
