# Source layers: common → foundation → lib → domain → feature → gui → app.
# Convenience INTERFACE `pp_base` = foundation + Amp + mesh + lib (feature/app link).
add_subdirectory(common)
add_subdirectory(foundation)
add_subdirectory(lib)

# Product-stack convenience (legacy name). Prefer linking specific pp_foundation_* /
# pp_lib_* / pp_amp_* / pp_mesh_* targets when a narrower dependency is enough.
add_library(pp_base INTERFACE)
target_link_libraries(pp_base INTERFACE
  pp_foundation
  pp_amp_l1
  pp_amp_l2
  pp_amp_l3
  pp_amp_link
  pp_mesh
  pp_lib
)

# Domain: capabilities composed from libs, shared by features (and pp-node / the probes).
add_subdirectory(domain)

if(NOT PP_BROWSER_HEADLESS)
  add_subdirectory(feature)
  add_subdirectory(gui)
endif()

if(PP_BROWSER_BUILD_TESTS)
  pp_browser_add_registered_tests()
endif()

add_subdirectory(app)
