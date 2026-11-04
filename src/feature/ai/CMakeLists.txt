pp_browser_add_feature_library(pp_feature_ai
  SOURCES
    AgentSession.cpp
    AgentSessions.cpp
    AgentUiPorts.cpp
    AiTurnPool.cpp
    TurnExecutor.cpp
  PUBLIC_LIBS
    pp_lib_ai_tools
    CURL::libcurl)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
