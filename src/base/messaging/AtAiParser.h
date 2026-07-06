#pragma once

#include <string>

namespace pbr {

enum class AtAiMode { None, Local, SharedReply, SharedFull };

struct AtAiParseResult {
  bool is_ai_invoke = false;
  AtAiMode mode = AtAiMode::None;
  std::string prompt;
};

AtAiParseResult ParseAtAiPrefix(const std::string& text);

} // namespace pbr
