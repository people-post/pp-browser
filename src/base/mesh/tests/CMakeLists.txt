pp_browser_add_base_folder_tests(pp_base_mesh pp_browser_mesh_test
  EXTRA_SOURCES
    ${CMAKE_SOURCE_DIR}/src/base/mesh/tests/support/mesh_harness_support.cpp
    ${CMAKE_SOURCE_DIR}/src/base/mesh/tests/support/mesh_test_harness.cpp
    ${CMAKE_SOURCE_DIR}/src/base/mesh/tests/support/mesh_triple_harness.cpp
  LINK_LIBS pp_domain_people pp_foundation_identity pp_domain_net)
