# Fetch people-post/pp-cpp-mesh (namespace pp::mesh, includes mesh/…): the mesh host and its
# services (docs/architecture/MESH.md). Targets: pp_mesh (all but the harness), pp_mesh_types (the
# vocabulary — pp-cpp-common only, for the app's lower layers), pp_mesh_<folder>, pp_mesh_testing.
#
# Pin a release tag from that repo's main line (PP_CPP_MESH_GIT_TAG); do not track branch tips.
# Optional override: PP_CPP_MESH_SOURCE_DIR (local checkout path). Include after pp-cpp-common,
# pp-cpp-crypto and pp-cpp-amp: the mesh takes those targets from here.

if(TARGET pp_mesh)
  return()
endif()

include(FetchContent)
include(PpFetchPin)

set(PP_CPP_MESH_SOURCE_DIR "" CACHE PATH
  "Optional local checkout of pp-cpp-mesh (overrides FetchContent)")
set(PP_CPP_MESH_GIT_REPOSITORY "https://github.com/people-post/pp-cpp-mesh.git"
  CACHE STRING "Git remote for pp-cpp-mesh")
pp_fetch_git_tag(PP_CPP_MESH_GIT_TAG "v1.0.0"
  "Release tag on pp-cpp-mesh main (not a branch name)")

# Mesh tests run in pp-cpp-mesh CI. UPnP stays off as it always was in pp-browser (turning it on is a
# product decision).
set(PP_MESH_BUILD_TESTS OFF CACHE BOOL "Build pp-cpp-mesh tests" FORCE)
set(PP_MESH_WITH_UPNP OFF CACHE BOOL "Map ports with UPnP (vendored miniupnpc)")
set(PP_MESH_WARNINGS_AS_ERRORS ${PP_BROWSER_WARNINGS_AS_ERRORS} CACHE BOOL
  "Treat compiler warnings as errors for pp-cpp-mesh targets")

if(PP_CPP_MESH_SOURCE_DIR AND EXISTS "${PP_CPP_MESH_SOURCE_DIR}/CMakeLists.txt")
  add_subdirectory("${PP_CPP_MESH_SOURCE_DIR}"
                   "${CMAKE_BINARY_DIR}/_deps/pp_cpp_mesh-build" EXCLUDE_FROM_ALL)
else()
  if(PP_CPP_MESH_SOURCE_DIR)
    message(WARNING "PP_CPP_MESH_SOURCE_DIR=${PP_CPP_MESH_SOURCE_DIR} is missing; falling back to FetchContent")
  endif()
  FetchContent_Declare(
    pp_cpp_mesh
    GIT_REPOSITORY ${PP_CPP_MESH_GIT_REPOSITORY}
    GIT_TAG ${PP_CPP_MESH_GIT_TAG}
  )
  FetchContent_MakeAvailable(pp_cpp_mesh)
endif()

foreach(_target pp_mesh pp_mesh_types pp_mesh_testing)
  if(NOT TARGET ${_target})
    message(FATAL_ERROR "pp-cpp-mesh did not define target ${_target}")
  endif()
endforeach()
