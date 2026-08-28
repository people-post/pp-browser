# Fetch people-post/pp-cpp-common (namespace pp) as target pp_common.
#
# Pin a release tag from that repo's main line (PP_CPP_COMMON_GIT_TAG).
# Do not track develop/main branch tips.

include(FetchContent)

set(PP_CPP_COMMON_GIT_REPOSITORY "https://github.com/people-post/pp-cpp-common.git"
  CACHE STRING "Git remote for pp-cpp-common")
set(PP_CPP_COMMON_GIT_TAG "v0.2.0"
  CACHE STRING "Release tag on pp-cpp-common main (not a branch name)")

# Shared lib tests are owned by that repo; do not build them inside pp-browser.
set(PP_COMMON_BUILD_TESTS OFF CACHE BOOL "Build pp-cpp-common tests" FORCE)

FetchContent_Declare(
  pp_cpp_common
  GIT_REPOSITORY ${PP_CPP_COMMON_GIT_REPOSITORY}
  GIT_TAG ${PP_CPP_COMMON_GIT_TAG}
)
FetchContent_MakeAvailable(pp_cpp_common)

if(NOT TARGET pp_common)
  message(FATAL_ERROR "pp-cpp-common did not define target pp_common")
endif()

# pp-browser call sites still use namespace pbr; force-include the slim bridge
# (PbrCompatForce.h) so ResultOrError/Logger/etc. aliases keep working.
# Value/Object/Json stay out of the force-include (explicit PbrCompat.h /
# ValueJson.h) — pulling variant document types into every TU broke MSVC.
#
# INTERFACE (not PUBLIC): do not force-include into pp_common's own TUs.
# MSVC /FI must be a separate argument from the path so D:/... roots are not
# misparsed as /FID:.
set(PP_BROWSER_PBR_COMPAT_FORCE_HEADER
    "${CMAKE_SOURCE_DIR}/src/common/PbrCompatForce.h")
if(MSVC)
  target_compile_options(pp_common INTERFACE "/FI"
                         "${PP_BROWSER_PBR_COMPAT_FORCE_HEADER}")
else()
  target_compile_options(pp_common INTERFACE "-include"
                         "${PP_BROWSER_PBR_COMPAT_FORCE_HEADER}")
endif()
