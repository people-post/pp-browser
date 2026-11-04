# The profile's content store (projects/content-cas C012): content-addressed private and public
# realms (CasStore over the IObjectIndex port; its SQLite one is the app's storage, CR024), the
# library, private content by hash. Chat attachment policy (views, suppression, the RAM cache)
# stays in messaging; features wire the two.
pp_browser_add_lib_library(pp_lib_content
  SOURCES
    CasLibrary.cpp
    CasStore.cpp
    PrivateContent.cpp
  PUBLIC_LIBS
    pp_foundation_error
    pp_foundation_crypto
    sodium)

if(PP_BROWSER_BUILD_TESTS)
  # The content index in memory: test binaries only, never linked into the app.
  pp_browser_add_lib_library(pp_lib_content_testing
    SOURCES
      testing/InMemoryObjectIndex.cpp
    PUBLIC_LIBS
      pp_lib_content)
  add_subdirectory(tests)
endif()
