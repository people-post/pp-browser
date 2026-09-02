include(PpBrowserBase)

# Domain peers (strict independence).
add_subdirectory(people)
# net is available in headless too (mesh PRIVATE-links it; pp-node uses HTTP helpers).
add_subdirectory(net)
# mesh is available in headless too (pp-node / dial / reachability).
add_subdirectory(mesh)
if(NOT PP_BROWSER_HEADLESS)
  add_subdirectory(messaging)
  add_subdirectory(ai)
  add_subdirectory(media)
  # Product shell (theme/catalogs). Host/overlays: foundation/platform/ui/.
  add_subdirectory(ui)
endif()
