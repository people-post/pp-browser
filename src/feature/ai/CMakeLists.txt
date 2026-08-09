add_subdirectory(tools)
add_subdirectory(bindings)

pp_browser_add_feature_library(pp_feature_ai
  SOURCES
    AgentSession.cpp
    PayloadTurnPlanBuilder.cpp
    ToolPermissionPolicy.cpp
    ToolPermissionPrompt.cpp
    ToolRegistry.cpp
    ToolRegistryBuild.cpp
    TurnExecutor.cpp
    TurnPlanner.cpp
    UiGenerator.cpp
  PUBLIC_LIBS
    pp_feature_ai_bindings
    pp_feature_ai_tools
    nlohmann_json::nlohmann_json
    CURL::libcurl)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
  add_subdirectory(bindings/tests)
endif()
