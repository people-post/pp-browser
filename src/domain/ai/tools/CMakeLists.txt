# The assistant's built-in tool providers (web search, MCP servers) and the registry built from them.
pp_browser_add_domain_library(pp_domain_ai_tools
  SOURCES
    McpToolAdapter.cpp
    McpToolProvider.cpp
    ToolRegistryBuild.cpp
    WebSearchProvider.cpp
    WebSearchTool.cpp
  PUBLIC_LIBS
    pp_domain_ai
    pp_domain_ai_mcp
    pp_foundation_data
    CURL::libcurl)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
