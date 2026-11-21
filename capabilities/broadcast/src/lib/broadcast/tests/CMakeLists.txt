# The signed formats' frozen vectors (release-compatibility SIGNED_FORMATS § 2.9, § 6) live in data/, beside this test.
pp_browser_add_lib_folder_tests(
  pp_lib_broadcast
  pp_browser_broadcast_test
  PRIVATE_DEFINITIONS
    PP_BROADCAST_JOIN_TICKET_VECTORS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/data/broadcast_join_ticket_vectors.json"
    PP_PEER_ANNOUNCE_TIP_VECTORS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/data/peer_announce_tip_vectors.json")
