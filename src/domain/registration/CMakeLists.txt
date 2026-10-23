include(PpBrowserDomain)

# Registration orchestration (N027): finish / renew / nickname over IRegistrationClient +
# IdentityStore. Headless — the desktop hub and pp-node share it.
pp_browser_add_domain_library(pp_domain_registration
  SOURCES
    RegistrationClient.cpp
  PUBLIC_LIBS
    pp_lib_net
    pp_lib_people)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
