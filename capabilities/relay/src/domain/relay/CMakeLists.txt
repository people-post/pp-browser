include(PpBrowserDomain)

# The relay services of pp-node-relay (projects/node-relay): none yet — NR3 adds the mailbox. Headless.
pp_browser_add_domain_library(pp_domain_relay
  SOURCES
    RelayServices.cpp
  PUBLIC_LIBS
    pp_mesh)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
