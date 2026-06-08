#pragma once

#include <string>
#include <vector>

namespace ppbrowser {

struct McpTool;

class PromptBuilder {
public:
  static std::string DefaultRcssProfile();
  static std::string ChatBlocksProfile();

  static std::string BuildUiGenerationPrompt(const std::string& tools_context,
                                             const std::string& rml_profile);

  static std::string BuildChatSystemPrompt();
};

} // namespace ppbrowser
