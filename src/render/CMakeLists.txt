# pp-browser render subtree: upstream-shaped RmlUi fork + pp-browser integration glue.
include(PpBrowserRender)

pp_browser_configure_rmlui_fork()

if(RMLUI_TESTS)
  enable_testing()
endif()
if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(fork)
else()
  add_subdirectory(fork EXCLUDE_FROM_ALL)
endif()

pp_browser_add_rmlui_backend()
pp_configure_status("RmlUi configured")
