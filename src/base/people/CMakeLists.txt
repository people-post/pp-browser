set(PP_BASE_PEOPLE_SOURCES
  AvatarGlyph.cpp
  ProfileIconCache.cpp
  ContactJson.cpp
  ContactIdentity.cpp
  ContactsStore.cpp
  Ed25519Signer.cpp
  IdentityStore.cpp
  LinkDeviceExchange.cpp
  MeshHopPolicy.cpp
  PeerDisplayLabel.cpp
)

pp_browser_add_base_library(pp_base_people
  SOURCES ${PP_BASE_PEOPLE_SOURCES}
  PUBLIC_LIBS
    pp_base_error
    pp_base_data
    pp_base_crypto
    pp_base_mesh_identity
    sodium)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
