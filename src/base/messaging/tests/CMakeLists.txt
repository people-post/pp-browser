add_executable(pp_browser_sqlite_thread_store_test sqlite_thread_store_test.cpp)
target_include_directories(pp_browser_sqlite_thread_store_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_sqlite_thread_store_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_base
  pp_common)
pp_browser_add_gtest(pp_browser_sqlite_thread_store_test)

add_executable(pp_browser_p2p_relay_wire_test p2p_relay_wire_test.cpp)
target_include_directories(pp_browser_p2p_relay_wire_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_p2p_relay_wire_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_base
  pp_common)
pp_browser_add_gtest(pp_browser_p2p_relay_wire_test)

add_executable(pp_browser_chat_payload_validator_test chat_payload_validator_test.cpp)
target_include_directories(pp_browser_chat_payload_validator_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_chat_payload_validator_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_base
  pp_common)
pp_browser_add_gtest(pp_browser_chat_payload_validator_test)

add_executable(pp_browser_v6_schema_test v6_schema_test.cpp)
target_include_directories(pp_browser_v6_schema_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_v6_schema_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_base
  pp_common)
pp_browser_add_gtest(pp_browser_v6_schema_test)
