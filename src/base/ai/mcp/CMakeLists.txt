pp_browser_add_base_library(pp_base_ai_mcp
  SOURCES
    McpClient.cpp
    McpRuntime.cpp
    SchemaAdapter.cpp
  PUBLIC_LIBS
    pp_base_ai
    pp_base_data
    pp_base_platform
    CURL::libcurl)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
