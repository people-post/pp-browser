add_executable(pp_browser_chat_history_responder_test chat_history_responder_test.cpp)
target_include_directories(pp_browser_chat_history_responder_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_chat_history_responder_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_feature
  pp_base
  pp_common)
pp_browser_add_gtest(pp_browser_chat_history_responder_test)

add_executable(pp_browser_chat_sync_test chat_sync_test.cpp)
target_include_directories(pp_browser_chat_sync_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_chat_sync_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_feature
  pp_base
  pp_common)
pp_browser_add_gtest(pp_browser_chat_sync_test)

add_executable(pp_browser_e2e_relay_crypto_test e2e_relay_crypto_test.cpp)
target_include_directories(pp_browser_e2e_relay_crypto_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_e2e_relay_crypto_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_feature
  pp_base
  pp_common)
pp_browser_add_gtest(pp_browser_e2e_relay_crypto_test)

add_executable(pp_browser_messaging_cross_cutting_test messaging_cross_cutting_test.cpp)
target_include_directories(pp_browser_messaging_cross_cutting_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_messaging_cross_cutting_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_feature
  pp_base
  pp_common)
pp_browser_add_gtest(pp_browser_messaging_cross_cutting_test)

add_executable(pp_browser_psk_session_coordinator_test psk_session_coordinator_test.cpp)
target_include_directories(pp_browser_psk_session_coordinator_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_psk_session_coordinator_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_feature
  pp_base
  pp_common)
pp_browser_add_gtest(pp_browser_psk_session_coordinator_test)
