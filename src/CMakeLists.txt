# Source layers: common → base → feature → app (executable in app/).
add_subdirectory(common)
add_subdirectory(base)
add_subdirectory(feature)

if(PP_BROWSER_BUILD_TESTS)
  pp_browser_add_registered_tests()
  add_subdirectory(${CMAKE_SOURCE_DIR}/tests ${CMAKE_BINARY_DIR}/tests)
endif()

add_subdirectory(app)
