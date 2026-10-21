add_subdirectory(bindings)

pp_browser_add_feature_library(pp_feature_ai
  SOURCES
    AgentSession.cpp
    AgentUiPorts.cpp
    TurnExecutor.cpp
  PUBLIC_LIBS
    pp_feature_ai_bindings
    pp_domain_ai_tools
    CURL::libcurl)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
  add_subdirectory(bindings/tests)
endif()
