# Add people-post/pp-cpp-mesh (namespace pp::mesh, includes mesh/…): the mesh host and its
# services (docs/architecture/MESH.md). Targets: pp_mesh (all but the harness), pp_mesh_types (the
# vocabulary — pp-cpp-common only, for the app's lower layers), pp_mesh_<folder>, pp_mesh_testing.
#
# At its pin (cmake/PpPins.cmake; PP_CPP_MESH_GIT_TAG, a release tag on that repo's main line), a local checkout
# (PP_CPP_MESH_SOURCE_DIR) or a sibling (PP_SIBLINGS_DIR). Include after pp-cpp-common, pp-cpp-crypto and
# pp-cpp-amp: the mesh takes those targets from here.

if(TARGET pp_mesh)
  return()
endif()

# Mesh tests run in pp-cpp-mesh CI. UPnP stays off as it always was in pp-browser (turning it on is a
# product decision).
set(PP_MESH_BUILD_TESTS OFF CACHE BOOL "Build pp-cpp-mesh tests" FORCE)
set(PP_MESH_WITH_UPNP OFF CACHE BOOL "Map ports with UPnP (vendored miniupnpc)")
set(PP_MESH_WARNINGS_AS_ERRORS ${PP_BROWSER_WARNINGS_AS_ERRORS} CACHE BOOL
  "Treat compiler warnings as errors for pp-cpp-mesh targets")

pp_pin_make_available(pp-cpp-mesh)

foreach(_target pp_mesh pp_mesh_types pp_mesh_testing)
  if(NOT TARGET ${_target})
    message(FATAL_ERROR "pp-cpp-mesh did not define target ${_target}")
  endif()
endforeach()
