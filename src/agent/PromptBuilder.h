#pragma once

#include <string>
#include <vector>

namespace ppbrowser {

struct McpTool;

class PromptBuilder {
public:
  static std::string DefaultRcssProfile();

  static std::string BuildUiGenerationPrompt(const std::string& tools_context,
                                             const std::string& rml_profile);

  static std::string BuildChatSystemPrompt(const std::string& format_spec);
};

} // namespace ppbrowser
