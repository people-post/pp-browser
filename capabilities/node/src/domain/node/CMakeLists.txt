include(PpBrowserDomain)

# Node-role mesh services: what a node hosts, its DHT / directory config and self row. Headless —
# the product's mesh (feature/product ProductMesh) and pp-node share it.
pp_browser_add_domain_library(pp_domain_node
  SOURCES
    NodeMeshServices.cpp
  PUBLIC_LIBS
    pp_mesh
    pp_lib_people
    pp_foundation_runtime_core)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
