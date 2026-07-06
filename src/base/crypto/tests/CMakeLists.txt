add_executable(pp_browser_crypto_vectors_test crypto_vectors_test.cpp)
target_include_directories(pp_browser_crypto_vectors_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_crypto_vectors_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_base
  pp_common)
pp_browser_add_gtest(pp_browser_crypto_vectors_test)

add_executable(pp_browser_psk_bundle_codec_test psk_bundle_codec_test.cpp)
target_include_directories(pp_browser_psk_bundle_codec_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_psk_bundle_codec_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_base
  pp_common)
pp_browser_add_gtest(pp_browser_psk_bundle_codec_test)

add_executable(pp_browser_hybrid_kem_test hybrid_kem_test.cpp)
target_include_directories(pp_browser_hybrid_kem_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_browser_hybrid_kem_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_base
  pp_common)
pp_browser_add_gtest(pp_browser_hybrid_kem_test)
