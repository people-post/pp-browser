pp_browser_add_lib_folder_tests(
  pp_lib_net
  pp_browser_net_test
  LINK_LIBS
    pp_lib_net_testing
    CURL::libcurl
    Asio::asio)
