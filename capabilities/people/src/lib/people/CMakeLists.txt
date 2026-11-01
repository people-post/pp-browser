set(PP_DOMAIN_PEOPLE_SOURCES
  AvatarGlyph.cpp
  ProfileIconCache.cpp
  AccountStateCodec.cpp
  ContactJson.cpp
  ContactIdentity.cpp
  DirectoryHitRank.cpp
  ContactReachability.cpp
  ContactsStore.cpp
  ContactAddressDisclosure.cpp
  IdentityStore.cpp
  LinkDeviceCoordinator.cpp
  LinkDeviceExchange.cpp
  AccountLocker.cpp
  RecoveryBundle.cpp
  ContactClock.cpp
  RetiredKeychainBackup.cpp
  MeshHopPolicy.cpp
  PeerAccountBook.cpp
  PeopleLookupFailure.cpp
  PeerDisplayLabel.cpp
  RegistrationStatus.cpp
  AccountCard.cpp
  DevicePeerId.cpp
  AccountCardRequest.cpp
  PeerKemKeyStore.cpp
  PeerSigningKeyStore.cpp
)

pp_browser_add_lib_library(pp_lib_people
  SOURCES ${PP_DOMAIN_PEOPLE_SOURCES}
  PUBLIC_LIBS
    pp_people_common
    pp_foundation_error
    pp_foundation_crypto
    pp_foundation_identity
    sodium)

if(NOT PP_BROWSER_HEADLESS)
  # Peers' cards in the profile database (SQLite): the app's, not the headless node's, which builds no SQLite.
  pp_browser_add_lib_library(pp_lib_people_cards
    SOURCES PeerCardStore.cpp
    PUBLIC_LIBS
      pp_lib_people
      SQLite::SQLite3)
endif()

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
