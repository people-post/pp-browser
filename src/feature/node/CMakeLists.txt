include(PpBrowserFeature)

# Node-role mesh services: what a node hosts, its DHT / directory config and self row; and the
# executors every mesh takes (AppRuntimeMeshExecutors). Headless — shared by the desktop hub and pp-node.
pp_browser_add_headless_feature_library(pp_feature_node
  SOURCES
    AppRuntimeMeshExecutors.cpp
    NodeMeshServices.cpp
  PUBLIC_LIBS
    pp_mesh
    pp_domain_people
    pp_foundation_data
    pp_foundation_runtime_core)

# pp-node's ledger role (pp-ledger relay / miner on the node's mesh runtime): its own library so only
# pp-node links pp-ledger, privately — its include tree stays out of every other target.
if(PP_BROWSER_WITH_LEDGER)
  pp_browser_add_headless_feature_library(pp_feature_node_ledger
    SOURCES
      NodeLedgerRole.cpp
    PUBLIC_LIBS
      pp_mesh)
  target_link_libraries(pp_feature_node_ledger PRIVATE pp_server)
  target_compile_definitions(pp_feature_node_ledger PUBLIC PP_BROWSER_HAS_LEDGER)
endif()

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
