#pragma once

#include <optional>
#include <string>

namespace pbr {

struct AtAiParseResult {
  bool is_ai_invoke = false;
  std::string prompt;
};

AtAiParseResult ParseAtAiPrefix(const std::string& text);

} // namespace pbr
