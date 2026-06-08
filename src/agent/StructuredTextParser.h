#pragma once

#include <string>

namespace pbr {

struct ParseResult {
  bool ok = false;
  std::string rml;
  std::string error;
};

class StructuredTextParser {
public:
  static std::string EscapeText(const std::string& text);
  static ParseResult ParseBlocksJson(const std::string& json);
  static ParseResult ParseFromLlmOutput(const std::string& llm_output);
};

} // namespace pbr
