# Delivery's own tests: sessions, envelopes, codecs and the relay client, their ports filled by test doubles.
pp_browser_add_lib_folder_tests(
  pp_lib_delivery
  pp_browser_delivery_test)
target_link_libraries(pp_browser_delivery_test PRIVATE pp_lib_delivery_testing Asio::asio)
