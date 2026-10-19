include(PpBrowserFeature)

# Node-role mesh services: what a node hosts, its DHT / directory config and self row; and the
# executors every mesh takes (AppRuntimeMeshExecutors). Headless — shared by the desktop hub and pp-node.
pp_browser_add_headless_feature_library(pp_feature_node
  SOURCES
    AppRuntimeMeshExecutors.cpp
    NodeMeshServices.cpp
  PUBLIC_LIBS
    pp_domain_mesh
    pp_domain_people
    pp_foundation_data
    pp_foundation_runtime_core)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
