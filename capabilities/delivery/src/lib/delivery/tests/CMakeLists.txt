# Delivery's own tests: sessions, envelopes, codecs and the relay client, their ports filled by test doubles; and the
# relay sign vectors the relay copies (data/relay_sign_vectors.json, multi-relay MR007), with the envelope's, the
# peer requests' and relay grant v2's (release-compatibility SIGNED_FORMATS § 2.1, § 2.7, § 2.4).
pp_browser_add_lib_folder_tests(
  pp_lib_delivery
  pp_browser_delivery_test
  PRIVATE_DEFINITIONS
    PP_RELAY_SIGN_VECTORS_PATH="${PP_SPECS_VECTORS_DIR}/relay_sign_vectors.json"
    PP_ENVELOPE_VECTORS_PATH="${PP_SPECS_VECTORS_DIR}/envelope_vectors.json"
    PP_PEER_REQUEST_VECTORS_PATH="${PP_SPECS_VECTORS_DIR}/peer_request_vectors.json"
    PP_RELAY_GRANT_VECTORS_PATH="${PP_SPECS_VECTORS_DIR}/relay_grant_vectors.json")
target_link_libraries(pp_browser_delivery_test PRIVATE pp_lib_delivery_testing pp_foundation_sockets)
