set(PP_BASE_PEOPLE_SOURCES
  ContactJson.cpp
  ContactsStore.cpp
  Ed25519Signer.cpp
  IdentityStore.cpp
  ${CMAKE_SOURCE_DIR}/src/libp2p/integration/host/Libp2pHost.cpp
  ${CMAKE_SOURCE_DIR}/src/libp2p/integration/host/PeerSessionManager.cpp
  ${CMAKE_SOURCE_DIR}/src/libp2p/integration/host/PeerIdUtil.cpp
)

pp_browser_add_base_library(pp_base_people
  SOURCES ${PP_BASE_PEOPLE_SOURCES}
  PUBLIC_LIBS
    pp_base_error
    pp_base_data
    pp_base_crypto
    nlohmann_json::nlohmann_json
    sodium
    p2p
    p2p_peer_id
    p2p_keys_proto)

pp_browser_add_base_p2p_includes(pp_base_people)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
