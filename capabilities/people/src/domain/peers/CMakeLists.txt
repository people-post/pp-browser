include(PpBrowserDomain)

# Who a peer is, for any feature: directory nicknames for strangers (DirectoryShadowCache), their
# signing / KEM keys from kept account cards and the directory (AccountKeyResolver), the account-card
# policy and this account's own cards (OwnCards). The directory is mesh's interface: no backend client.
pp_browser_add_domain_library(pp_domain_peers
  SOURCES
    DirectoryShadowCache.cpp
    AccountKeyResolver.cpp
    AccountCardPolicy.cpp
    OwnCards.cpp
  PUBLIC_LIBS
    pp_lib_people
    pp_mesh
    pp_foundation_runtime_core)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
