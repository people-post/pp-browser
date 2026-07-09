add_executable(pp_browser_peer_id_util_test peer_id_util_test.cpp)
target_include_directories(pp_browser_peer_id_util_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_peer_id_util_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_base
  pp_common)
pp_browser_add_gtest(pp_browser_peer_id_util_test)

add_executable(pp_browser_identity_store_test identity_store_test.cpp)
target_include_directories(pp_browser_identity_store_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_identity_store_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_base
  pp_common
  nlohmann_json::nlohmann_json)
pp_browser_add_gtest(pp_browser_identity_store_test)
