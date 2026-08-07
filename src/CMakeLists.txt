# Source layers: common → base → feature → app (executable in app/).
add_subdirectory(common)
add_subdirectory(base)
if(NOT PP_BROWSER_HEADLESS)
  add_subdirectory(feature)
endif()

if(PP_BROWSER_BUILD_TESTS)
  pp_browser_add_registered_tests()
endif()

add_subdirectory(app)
