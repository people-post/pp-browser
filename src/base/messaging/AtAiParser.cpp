#include "base/messaging/AtAiParser.h"

#include <cctype>
#include <regex>

namespace pbr {

namespace {

std::string Trim(const std::string& text) {
  size_t start = 0;
  while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) {
    ++start;
  }
  size_t end = text.size();
  while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return text.substr(start, end - start);
}

} // namespace

AtAiParseResult ParseAtAiPrefix(const std::string& text) {
  static const std::regex pattern(R"(^@ai\s+(.+)$)", std::regex::icase);
  const std::string trimmed = Trim(text);
  std::smatch match;
  if (std::regex_match(trimmed, match, pattern) && match.size() >= 2) {
    return AtAiParseResult{.is_ai_invoke = true, .prompt = Trim(match[1].str())};
  }
  return {};
}

} // namespace pbr
