#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace pbr {

struct ParsedSuggestion {
  std::string label;
  std::string message;
};

struct ParseResult {
  bool ok = false;
  std::string rml;
  std::vector<ParsedSuggestion> suggestions;
  std::string error;
};

struct EmbeddedToolCall {
  std::string name;
  nlohmann::json arguments;
};

class StructuredTextParser {
public:
  static std::string EscapeText(const std::string& text);
  static std::string EscapeExpressionString(const std::string& text);
  static ParseResult ParseBlocksJson(const std::string& json);
  static ParseResult ParseFromLlmOutput(const std::string& llm_output);

  // Models without native tool calling sometimes embed {"tool":"...", "params":{...}} in blocks.
  static std::optional<std::vector<EmbeddedToolCall>> ExtractEmbeddedToolCalls(const std::string& llm_output);
};

} // namespace pbr
