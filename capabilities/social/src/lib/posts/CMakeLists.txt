# Public posts (projects/social-posts): per-device lanes of signed, hash-chained entries, head
# statements, the profile's post store, and the device's writer. No peers: keys come from callers.
pp_browser_add_lib_library(pp_lib_posts
  SOURCES
    PostAuthor.cpp
    PostBodies.cpp
    PostEntry.cpp
    PostStore.cpp
  PUBLIC_LIBS
    pp_foundation_crypto
    sodium
    SQLite::SQLite3)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
