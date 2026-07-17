pp_browser_add_feature_library(pp_feature_ai_tools
  SOURCES
    McpToolAdapter.cpp
    WebSearchTool.cpp
  PUBLIC_LIBS
    nlohmann_json::nlohmann_json
    CURL::libcurl)
