include(PpBrowserBase)

# Domain peers (strict independence).
add_subdirectory(people)
# net is available in headless too (mesh PRIVATE-links it; pp-node uses HTTP helpers).
add_subdirectory(net)
# mesh is available in headless too (pp-node / dial / reachability).
add_subdirectory(mesh)

set(_pp_domain_peers
  pp_domain_people
  pp_domain_net
  pp_domain_mesh
)

if(NOT PP_BROWSER_HEADLESS)
  add_subdirectory(messaging)
  add_subdirectory(ai)
  add_subdirectory(media)
  # Product shell (theme/catalogs). Host/overlays: foundation/platform/ui/.
  add_subdirectory(ui)
  list(APPEND _pp_domain_peers
    pp_domain_messaging
    pp_domain_ai
    pp_domain_ai_conversation
    pp_domain_ai_mcp
    pp_domain_media
    pp_domain_ui
  )
endif()

# Aggregate for this build's domain peers (mirrors pp_foundation).
add_library(pp_domain INTERFACE)
target_link_libraries(pp_domain INTERFACE ${_pp_domain_peers})
unset(_pp_domain_peers)
