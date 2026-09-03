# Source layers: common → foundation → domain → feature → gui → app.
# Convenience INTERFACE `pp_base` = foundation + Amp + domain peers (feature/app link).
add_subdirectory(common)
add_subdirectory(foundation)
add_subdirectory(domain)

# Product-stack convenience (legacy name). Prefer linking specific pp_foundation_* /
# pp_domain_* / pp_amp_* targets when a narrower dependency is enough.
add_library(pp_base INTERFACE)
target_link_libraries(pp_base INTERFACE
  pp_foundation
  pp_amp_l1
  pp_amp_l2
  pp_amp_l3
  pp_amp_link
)

# Domain peers attach onto pp_base after both trees exist.
if(TARGET pp_domain_people)
  target_link_libraries(pp_base INTERFACE pp_domain_people)
endif()
if(TARGET pp_domain_media)
  target_link_libraries(pp_base INTERFACE pp_domain_media)
endif()
if(TARGET pp_domain_net)
  target_link_libraries(pp_base INTERFACE pp_domain_net)
endif()
if(TARGET pp_domain_ui)
  target_link_libraries(pp_base INTERFACE pp_domain_ui)
endif()
if(TARGET pp_domain_messaging)
  target_link_libraries(pp_base INTERFACE pp_domain_messaging)
endif()
if(TARGET pp_domain_ai)
  target_link_libraries(pp_base INTERFACE pp_domain_ai)
endif()
if(TARGET pp_domain_ai_conversation)
  target_link_libraries(pp_base INTERFACE pp_domain_ai_conversation)
endif()
if(TARGET pp_domain_ai_mcp)
  target_link_libraries(pp_base INTERFACE pp_domain_ai_mcp)
endif()
if(TARGET pp_domain_mesh)
  target_link_libraries(pp_base INTERFACE pp_domain_mesh)
endif()

# Optional domain aggregate (same peers as attached above; for callers that want
# domain without re-listing). Defined only after peers exist.
add_library(pp_domain INTERFACE)
foreach(_peer
    pp_domain_people
    pp_domain_media
    pp_domain_net
    pp_domain_ui
    pp_domain_messaging
    pp_domain_ai
    pp_domain_ai_conversation
    pp_domain_ai_mcp
    pp_domain_mesh)
  if(TARGET ${_peer})
    target_link_libraries(pp_domain INTERFACE ${_peer})
  endif()
endforeach()

if(NOT PP_BROWSER_HEADLESS)
  add_subdirectory(feature)
  add_subdirectory(gui)
endif()

if(PP_BROWSER_BUILD_TESTS)
  pp_browser_add_registered_tests()
endif()

add_subdirectory(app)
