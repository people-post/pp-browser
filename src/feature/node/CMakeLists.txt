include(PpBrowserFeature)

# Node-role mesh services: what a node hosts, its DHT / directory config and self row. Headless —
# shared by the desktop hub (MeshRole::Node) and pp-node.
pp_browser_add_headless_feature_library(pp_feature_node
  SOURCES
    NodeMeshServices.cpp
  PUBLIC_LIBS
    pp_domain_mesh
    pp_domain_people
    pp_foundation_data)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
