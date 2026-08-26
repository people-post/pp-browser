# Source layers: common (via FetchContent pp-cpp-common) → base → feature → app.
add_subdirectory(base)
if(NOT PP_BROWSER_HEADLESS)
  add_subdirectory(feature)
endif()

if(PP_BROWSER_BUILD_TESTS)
  pp_browser_add_registered_tests()
endif()

add_subdirectory(app)
