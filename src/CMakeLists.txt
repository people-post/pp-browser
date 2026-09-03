# Source layers: common → foundation → domain → feature → gui → app.
# Convenience INTERFACE `pp_base` = foundation + Amp + domain (feature/app link).
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
  pp_domain
)

if(NOT PP_BROWSER_HEADLESS)
  add_subdirectory(feature)
  add_subdirectory(gui)
endif()

if(PP_BROWSER_BUILD_TESTS)
  pp_browser_add_registered_tests()
endif()

add_subdirectory(app)
