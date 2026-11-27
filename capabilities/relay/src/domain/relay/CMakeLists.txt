include(PpBrowserDomain)

# The message relay, an add-on of pp-node-plus (projects/node-relay): empty until NR3 adds the mailbox. Headless.
pp_browser_add_domain_library(pp_domain_relay
  SOURCES
    RelayService.cpp
  PUBLIC_LIBS
    pp_mesh)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
