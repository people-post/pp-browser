#pragma once

#include <string>

namespace ppbrowser {

struct ParseResult {
  bool ok = false;
  std::string rml;
  std::string error;
};

class StructuredTextParser {
public:
  static ParseResult ParseBlocksJson(const std::string& json);
  static ParseResult ParseFromLlmOutput(const std::string& llm_output);
};

} // namespace ppbrowser
