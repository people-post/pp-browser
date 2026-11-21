# relay_certificate_vectors.json (release-compatibility SIGNED_FORMATS § 6): written here, read byte for byte by
# pp-relay-apis and pp-node (docs/contracts/SIGNED_EXTENSIONS.md § Vector files).
pp_browser_add_lib_folder_tests(
  pp_lib_net
  pp_browser_net_test
  PRIVATE_DEFINITIONS
    PP_RELAY_CERTIFICATE_VECTORS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/data/relay_certificate_vectors.json"
  LINK_LIBS
    pp_lib_net_testing
    CURL::libcurl
    Asio::asio)
