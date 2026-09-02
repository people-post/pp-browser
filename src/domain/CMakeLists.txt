include(PpBrowserBase)

# Domain peers (strict independence). Migrating from src/base/ → src/domain/.
add_subdirectory(people)
# net is available in headless too (mesh PRIVATE-links it; pp-node uses HTTP helpers).
add_subdirectory(net)
if(NOT PP_BROWSER_HEADLESS)
  add_subdirectory(media)
endif()
