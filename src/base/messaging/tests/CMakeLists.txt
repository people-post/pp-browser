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

add_executable(pp_browser_envelope_signer_test envelope_signer_test.cpp)
target_include_directories(pp_browser_envelope_signer_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_envelope_signer_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_base
  pp_common)
pp_browser_add_gtest(pp_browser_envelope_signer_test)

add_executable(pp_browser_e2e_ingest_classifier_test e2e_ingest_classifier_test.cpp)
target_include_directories(pp_browser_e2e_ingest_classifier_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_e2e_ingest_classifier_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_base
  pp_common)
pp_browser_add_gtest(pp_browser_e2e_ingest_classifier_test)

add_executable(pp_browser_v6_pipeline_test v6_pipeline_test.cpp)
target_include_directories(pp_browser_v6_pipeline_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_v6_pipeline_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_base
  pp_common)
pp_browser_add_gtest(pp_browser_v6_pipeline_test)

add_executable(pp_browser_relay_history_test relay_history_test.cpp)
target_include_directories(pp_browser_relay_history_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_relay_history_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_base
  pp_common)
pp_browser_add_gtest(pp_browser_relay_history_test)

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

add_executable(pp_browser_v6_integrity_test v6_integrity_test.cpp)
target_include_directories(pp_browser_v6_integrity_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_v6_integrity_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_base
  pp_common)
pp_browser_add_gtest(pp_browser_v6_integrity_test)

add_executable(pp_browser_e2e_relay_crypto_test e2e_relay_crypto_test.cpp)
target_include_directories(pp_browser_e2e_relay_crypto_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_e2e_relay_crypto_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_feature
  pp_base
  pp_common)
pp_browser_add_gtest(pp_browser_e2e_relay_crypto_test)
