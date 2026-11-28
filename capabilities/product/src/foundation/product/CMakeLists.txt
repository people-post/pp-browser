# The build's identity (CR015 product base, CR023 step 3; capabilities/product since CR028): its version
# (AppVersion.h), the commit and tag it was built from (BuildInfo.h) and the dev-tool switches (DevTools.h). The apps
# read it and hand capabilities and the base values (CrashIdentity, the text layer's {product}); capabilities never
# include it. The product's name and slug (ProductBranding.h) are the desktop app's, in src/foundation/product.

# A tag the build was given (the release workflow or a script passes it, e.g. internal/141). Empty: the
# PP_BUILD_TAG environment variable at build time, then a build tag on a clean HEAD; none: About shows only the commit.
set(PP_BUILD_TAG "" CACHE STRING "Build tag shown in About (e.g. internal/141); empty = PP_BUILD_TAG env, then git")
# The commit the build was given (a hex hash, e.g. the workflow's github.sha). Empty: the PP_BUILD_COMMIT environment
# variable at build time, then git; "unknown" without either.
set(PP_BUILD_COMMIT "" CACHE STRING "Commit shown in About (hex hash); empty = PP_BUILD_COMMIT env, then git")

# The commit comes from a target that runs on EVERY build (CMake script mode, no shell), not from configure:
# the generated file is rewritten only when its text changes, so only BuildInfoGenerated.cpp recompiles.
set(PP_BUILD_INFO_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/BuildInfoGenerated.cpp")
add_custom_target(pp_build_info_generate
  COMMAND ${CMAKE_COMMAND}
    "-DSOURCE_DIR=${PP_BROWSER_ROOT}"
    "-DOUTPUT=${PP_BUILD_INFO_GENERATED}"
    "-DBUILD_TAG=${PP_BUILD_TAG}"
    "-DBUILD_COMMIT=${PP_BUILD_COMMIT}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/GenBuildInfo.cmake"
  BYPRODUCTS "${PP_BUILD_INFO_GENERATED}"
  COMMENT "Generating build info"
  VERBATIM)

pp_browser_add_foundation_library(pp_foundation_product
  SOURCES
    BuildInfo.cpp
    "${PP_BUILD_INFO_GENERATED}")
add_dependencies(pp_foundation_product pp_build_info_generate)
target_compile_definitions(pp_foundation_product PUBLIC
  "PP_BROWSER_RELEASE_VERSION=\"${PP_BROWSER_RELEASE_VERSION}\""
  "PP_BROWSER_VERSION=\"${PP_BROWSER_VERSION}\"")

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
