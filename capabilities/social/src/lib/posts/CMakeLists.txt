# Public posts (projects/social-posts): per-device lanes of signed, hash-chained entries, head
# statements, the profile's post store, and the device's writer. No peers: keys come from callers.
pp_browser_add_lib_library(pp_lib_posts
  SOURCES
    PostAuthor.cpp
    PostBodies.cpp
    PostEntry.cpp
    IPostStore.cpp
  PUBLIC_LIBS
    pp_foundation_crypto
    sodium)

if(PP_BROWSER_BUILD_TESTS)
  # Posts in memory: test binaries only, never linked into the app.
  pp_browser_add_lib_library(pp_lib_posts_testing
    SOURCES
      testing/InMemoryPostStore.cpp
    PUBLIC_LIBS
      pp_lib_posts)
  add_subdirectory(tests)
endif()
