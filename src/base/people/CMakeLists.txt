set(PP_BASE_PEOPLE_SOURCES
  ContactJson.cpp
  ContactsStore.cpp
  Ed25519Signer.cpp
  IdentityStore.cpp
  PeerDisplayLabel.cpp
)

pp_browser_add_base_library(pp_base_people
  SOURCES ${PP_BASE_PEOPLE_SOURCES}
  PUBLIC_LIBS
    pp_base_error
    pp_base_data
    pp_base_crypto
    nlohmann_json::nlohmann_json
    sodium
    pp_libp2p_integration)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
