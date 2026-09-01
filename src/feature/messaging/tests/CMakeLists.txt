pp_browser_add_feature_folder_tests(
  pp_feature_messaging
  pp_browser_feature_messaging_test
  EXTRA_SOURCES
    ${CMAKE_SOURCE_DIR}/src/base/mesh/tests/support/mesh_harness_support.cpp
    ${CMAKE_SOURCE_DIR}/src/base/mesh/tests/support/mesh_test_harness.cpp
  PRIVATE_DEFINITIONS
    PP_BROWSER_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")
