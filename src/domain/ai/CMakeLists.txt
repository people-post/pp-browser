pp_browser_add_domain_library(pp_domain_ai
  SOURCES
    BriefAiClient.cpp
    LlmClient.cpp
    MarkdownToRml.cpp
    PromptBuilder.cpp
    PayloadTurnPlanBuilder.cpp
    RmlValidator.cpp
    SseClient.cpp
    SseParser.cpp
    StructuredTextParser.cpp
    ToolRegistry.cpp
    ToolResultFormatter.cpp
    TurnPlan.cpp
    TurnTrace.cpp
    WorkingSetPolicy.cpp
  PUBLIC_LIBS
    pp_foundation_error
    pp_foundation_platform_core
    CURL::libcurl)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()

add_subdirectory(conversation)
add_subdirectory(mcp)
