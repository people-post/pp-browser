set(PP_DOMAIN_PEOPLE_SOURCES
  DirectChatTargetFromContact.cpp
  AvatarGlyph.cpp
  ProfileIconCache.cpp
  AccountBackupCodec.cpp
  ContactJson.cpp
  ContactIdentity.cpp
  ContactReachability.cpp
  ContactsStore.cpp
  ContactAddressDisclosure.cpp
  Ed25519Signer.cpp
  IdentityStore.cpp
  LinkDeviceExchange.cpp
  MeshHopPolicy.cpp
  PeerAccountBook.cpp
  PeerBriefRoute.cpp
  PeerDisplayLabel.cpp
  RegistrationStatus.cpp
)

pp_browser_add_domain_library(pp_domain_people
  SOURCES ${PP_DOMAIN_PEOPLE_SOURCES}
  PUBLIC_LIBS
    pp_foundation_error
    pp_foundation_data
    pp_foundation_crypto
    pp_foundation_identity
    pp_foundation_platform_core
    sodium)

# Ed25519Signer uses OpenSSL's EVP Ed25519 (the vendored BoringSSL's does not generate those keys).
if(TARGET OpenSSL::Crypto)
  target_link_libraries(pp_domain_people PUBLIC OpenSSL::Crypto)
endif()

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
