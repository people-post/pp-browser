pp_browser_add_base_library(pp_base_ai
  SOURCES
    LlmClient.cpp
    PromptBuilder.cpp
    RmlValidator.cpp
    StructuredTextParser.cpp
    ToolResultFormatter.cpp
    TurnPlan.cpp
    TurnTrace.cpp
    WorkingSetPolicy.cpp
  PUBLIC_LIBS
    pp_base_error
    pp_base_messaging
    nlohmann_json::nlohmann_json
    CURL::libcurl)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()

add_subdirectory(conversation)
add_subdirectory(mcp)
