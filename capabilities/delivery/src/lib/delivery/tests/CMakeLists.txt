# Delivery's own tests: sessions, envelopes, codecs and the relay client, their ports filled by test doubles; and the
# relay sign vectors the relay copies (data/relay_sign_vectors.json, multi-relay MR007), with the envelope's and the
# peer requests' (release-compatibility SIGNED_FORMATS § 2.1, § 2.7).
pp_browser_add_lib_folder_tests(
  pp_lib_delivery
  pp_browser_delivery_test
  PRIVATE_DEFINITIONS
    PP_RELAY_SIGN_VECTORS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/data/relay_sign_vectors.json"
    PP_ENVELOPE_VECTORS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/data/envelope_vectors.json"
    PP_PEER_REQUEST_VECTORS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/data/peer_request_vectors.json")
target_link_libraries(pp_browser_delivery_test PRIVATE pp_lib_delivery_testing Asio::asio)
