add_subdirectory(tools)
add_subdirectory(bindings)

pp_browser_add_feature_library(pp_feature_ai
  SOURCES
    AgentSession.cpp
    AgentUiPorts.cpp
    PayloadTurnPlanBuilder.cpp
    ToolPermissionPolicy.cpp
    ToolPermissionPrompt.cpp
    ToolRegistryBuild.cpp
    TurnExecutor.cpp
    TurnPlanner.cpp
    UiGenerator.cpp
  PUBLIC_LIBS
    pp_feature_ai_bindings
    pp_feature_ai_tools
    CURL::libcurl)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
  add_subdirectory(bindings/tests)
endif()
