include(PpBrowserFeature)

# Registration orchestration (N027): finish / renew / nickname over IRegistrationClient +
# IdentityStore. Headless — shared by the desktop hub and pp-node.
pp_browser_add_headless_feature_library(pp_feature_registration
  SOURCES
    RegistrationClient.cpp
  PUBLIC_LIBS
    pp_domain_net
    pp_domain_people)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
