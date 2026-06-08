#pragma once

#include <string>
#include <vector>

namespace ppbrowser {

struct McpTool;

class PromptBuilder {
public:
  static std::string BuildUiGenerationPrompt(const std::string& tools_context,
                                             const std::string& rml_profile);
};

} // namespace ppbrowser
