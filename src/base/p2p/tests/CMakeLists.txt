pp_browser_add_base_folder_tests(pp_base_p2p pp_browser_p2p_test
  EXTRA_SOURCES
    ${CMAKE_SOURCE_DIR}/src/base/p2p/tests/support/mesh_harness_support.cpp
    ${CMAKE_SOURCE_DIR}/src/base/p2p/tests/support/mesh_test_harness.cpp
    ${CMAKE_SOURCE_DIR}/src/base/p2p/tests/support/mesh_triple_harness.cpp
  LINK_LIBS pp_base_people pp_base_peer_id)
