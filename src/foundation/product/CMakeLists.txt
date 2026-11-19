# The product's identity (CR015 product base, CR023 step 3): its name and slug (ProductBranding.h), the
# build's version (AppVersion.h) and the commit and tag it was built from (BuildInfo.h). The apps read it and hand
# capabilities and the base values (CrashIdentity, the text layer's {product}); capabilities never include it.

# A tag the build was given (the release workflow or a script passes it, e.g. internal/141). Empty: the
# PP_BUILD_TAG environment variable at build time; none: About shows only the commit.
set(PP_BUILD_TAG "" CACHE STRING "Build tag shown in About (e.g. internal/141); empty = none")

# The commit comes from a target that runs on EVERY build (CMake script mode, no shell), not from configure:
# the generated file is rewritten only when its text changes, so only BuildInfoGenerated.cpp recompiles.
set(PP_BUILD_INFO_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/BuildInfoGenerated.cpp")
add_custom_target(pp_build_info_generate
  COMMAND ${CMAKE_COMMAND}
    "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
    "-DOUTPUT=${PP_BUILD_INFO_GENERATED}"
    "-DBUILD_TAG=${PP_BUILD_TAG}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/GenBuildInfo.cmake"
  BYPRODUCTS "${PP_BUILD_INFO_GENERATED}"
  COMMENT "Generating build info"
  VERBATIM)

pp_browser_add_foundation_library(pp_foundation_product
  SOURCES
    BuildInfo.cpp
    "${PP_BUILD_INFO_GENERATED}")
add_dependencies(pp_foundation_product pp_build_info_generate)
target_include_directories(pp_foundation_product PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(pp_foundation_product PUBLIC
  "PP_BROWSER_RELEASE_VERSION=\"${PP_BROWSER_RELEASE_VERSION}\""
  "PP_BROWSER_VERSION=\"${PP_BROWSER_VERSION}\"")

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
