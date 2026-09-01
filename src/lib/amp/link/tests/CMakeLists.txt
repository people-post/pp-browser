pp_browser_add_base_folder_tests(pp_base_mesh_link pp_browser_amp_link_test
  EXTRA_SOURCES
    ${CMAKE_SOURCE_DIR}/src/base/mesh/tests/support/mesh_harness_support.cpp
    ${CMAKE_SOURCE_DIR}/src/base/mesh/tests/support/mesh_test_harness.cpp
  LINK_LIBS pp_base_peer_id)
