set(PP_DOMAIN_PEOPLE_SOURCES
  DirectChatTargetFromContact.cpp
  AvatarGlyph.cpp
  ProfileIconCache.cpp
  ProfileIconFetchUtil.cpp
  ContactJson.cpp
  ContactIdentity.cpp
  ContactReachability.cpp
  ContactsStore.cpp
  Ed25519Signer.cpp
  IdentityStore.cpp
  LinkDeviceExchange.cpp
  MeshHopPolicy.cpp
  PeerBriefRoute.cpp
  PeerDisplayLabel.cpp
)

pp_browser_add_domain_library(pp_domain_people
  SOURCES ${PP_DOMAIN_PEOPLE_SOURCES}
  PUBLIC_LIBS
    pp_foundation_error
    pp_foundation_data
    pp_foundation_crypto
    pp_foundation_identity
    sodium)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
