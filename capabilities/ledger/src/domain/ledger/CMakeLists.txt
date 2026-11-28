include(PpBrowserDomain)

# pp-node's ledger role (pp-ledger platform-integration pi4): one pp-ledger relay or miner on the node's mesh runtime.
# Only node builds link it, and pp-ledger privately, so its include tree stays out of every other target. Whichever
# node build links it starts it (apps/node/src/app/node/main.cpp, PP_NODE_HAS_LEDGER).
pp_browser_add_domain_library(pp_domain_ledger
  SOURCES
    LedgerRole.cpp
  PUBLIC_LIBS
    pp_mesh)
target_link_libraries(pp_domain_ledger PRIVATE pp_server)
target_compile_definitions(pp_domain_ledger PUBLIC PP_NODE_HAS_LEDGER)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
