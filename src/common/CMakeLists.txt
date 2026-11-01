include(PpBrowserWarnings)

# pp-browser cross-module utilities (FetchContent pp_common is the external common layer).
add_library(pp_pbr_common STATIC
  ImageMetadata.cpp
  Localized.cpp
  IPushDeviceRegistrar.cpp
  MeshConfig.cpp
  SchemaVersion.cpp
  ServiceEndpoints.cpp
  ui/WorkingSetCodec.cpp
  BoundedRead.cpp
  LengthPrefixedCodec.cpp
  ShareableLine.cpp
)
target_include_directories(pp_pbr_common PUBLIC ${CMAKE_SOURCE_DIR}/src)
# pp_mesh_types: the mesh vocabulary (directory rows, hops, relay scope, disclosure) app types embed.
target_link_libraries(pp_pbr_common PUBLIC pp_common pp_crypto pp_mesh_types)
pp_browser_apply_warnings(pp_pbr_common)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
  # Tests filed under their capability's common/ subdir (SRC_LAYOUT § Capabilities).
  add_subdirectory(ui/tests)
endif()
