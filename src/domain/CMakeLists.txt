include(PpBrowserBase)

# Domain peers (strict independence). Migrating from src/base/ → src/domain/.
add_subdirectory(people)
if(NOT PP_BROWSER_HEADLESS)
  add_subdirectory(media)
endif()
