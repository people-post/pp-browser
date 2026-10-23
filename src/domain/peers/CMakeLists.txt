include(PpBrowserDomain)

# Who a peer is, for any feature: directory nicknames for strangers (DirectoryShadowCache) and the
# names threads and senders show (PeerDisplaySources), their signing / KEM keys from kept account
# cards and the directory (AccountKeyResolver), profile icons; and the people → messaging bridges messaging cannot include
# (LocalKeysFromIdentity, GroupInviteStanding).
pp_browser_add_domain_library(pp_domain_peers
  SOURCES
    DirectoryShadowCache.cpp
    ProfileIconClient.cpp
    ProfileIconFetch.cpp
    AccountKeyResolver.cpp
    OwnKeyCard.cpp
  PUBLIC_LIBS
    pp_lib_people
    pp_lib_net
    pp_lib_messaging
    pp_mesh
    pp_foundation_runtime_core)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
