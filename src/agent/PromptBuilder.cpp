#include "agent/PromptBuilder.h"

#include <sstream>

namespace ppbrowser {

std::string PromptBuilder::BuildUiGenerationPrompt(const std::string& tools_context,
                                                   const std::string& rml_profile) {
  std::ostringstream out;
  out << "You generate RML, RCSS, and a bindings JSON manifest for pp-browser.\n\n";
  out << "RML profile rules:\n" << rml_profile << "\n\n";
  out << "Available MCP tools:\n" << tools_context << "\n\n";
  out << "Respond with three fenced blocks in order: ```rml, ```rcss, ```json (bindings manifest).\n";
  out << "Use data-model, data-value, data-event-click, and data-for. No script tags or inline JS.\n";
  return out.str();
}

} // namespace ppbrowser
