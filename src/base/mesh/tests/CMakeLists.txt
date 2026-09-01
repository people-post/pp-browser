if(NOT PP_BROWSER_BUILD_TESTS)
  return()
endif()

set(_PP_AMP_MESH_SUPPORT_DIR ${CMAKE_SOURCE_DIR}/src/base/mesh/tests/support)
set(PP_AMP_MESH_SUPPORT_SOURCES
  ${_PP_AMP_MESH_SUPPORT_DIR}/mesh_harness_support.cpp
  ${_PP_AMP_MESH_SUPPORT_DIR}/mesh_test_harness.cpp
  ${_PP_AMP_MESH_SUPPORT_DIR}/mesh_triple_harness.cpp
  ${_PP_AMP_MESH_SUPPORT_DIR}/amp_integration_harness.cpp)

add_subdirectory(integration)
