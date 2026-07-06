add_executable(pp_browser_service_client_factory_test service_client_factory_test.cpp)
target_include_directories(pp_browser_service_client_factory_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_service_client_factory_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_base
  pp_common
  nlohmann_json::nlohmann_json
  CURL::libcurl)
pp_browser_add_gtest(pp_browser_service_client_factory_test)

add_executable(pp_browser_registration_sign_payload_test registration_sign_payload_test.cpp)
target_include_directories(pp_browser_registration_sign_payload_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_registration_sign_payload_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_base
  pp_common)
pp_browser_add_gtest(pp_browser_registration_sign_payload_test)

add_executable(pp_browser_relay_api_sign_payload_test relay_api_sign_payload_test.cpp)
target_include_directories(pp_browser_relay_api_sign_payload_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_relay_api_sign_payload_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_base
  pp_common)
pp_browser_add_gtest(pp_browser_relay_api_sign_payload_test)
