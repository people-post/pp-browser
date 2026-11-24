# Add people-post/pp-cpp-common (namespace pp) as target pp_common, at its pin (cmake/PpPins.cmake;
# PP_CPP_COMMON_GIT_TAG, a release tag on that repo's main line), a local checkout (PP_CPP_COMMON_SOURCE_DIR) or a
# sibling (PP_SIBLINGS_DIR).

if(TARGET pp_common)
  return()
endif()

# Shared lib tests are owned by that repo; do not build them inside pp-browser.
set(PP_COMMON_BUILD_TESTS OFF CACHE BOOL "Build pp-cpp-common tests" FORCE)

pp_pin_make_available(pp-cpp-common)

if(NOT TARGET pp_common)
  message(FATAL_ERROR "pp-cpp-common did not define target pp_common")
endif()
