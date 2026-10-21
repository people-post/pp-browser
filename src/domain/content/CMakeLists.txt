# The profile's content store (projects/content-cas C012): content-addressed private and public
# realms (CasStore, ObjectIndex), the library, private content by hash. Chat attachment policy
# (views, suppression, the RAM cache) stays in messaging; features wire the two.
pp_browser_add_domain_library(pp_domain_content
  SOURCES
    CasLibrary.cpp
    CasStore.cpp
    ObjectIndex.cpp
    PrivateContent.cpp
  PUBLIC_LIBS
    pp_foundation_error
    pp_foundation_data
    pp_foundation_crypto
    sodium
    SQLite::SQLite3)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
