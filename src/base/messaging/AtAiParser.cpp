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

AtAiParseResult MatchPrompt(const std::string& trimmed, const std::regex& pattern, const AtAiMode mode) {
  std::smatch match;
  if (std::regex_match(trimmed, match, pattern) && match.size() >= 2) {
    return AtAiParseResult{.is_ai_invoke = true, .mode = mode, .prompt = Trim(match[1].str())};
  }
  return {};
}

} // namespace

AtAiParseResult ParseAtAiPrefix(const std::string& text) {
  const std::string trimmed = Trim(text);
  if (auto result = MatchPrompt(trimmed, std::regex(R"(^@ai\+\+\s+(.+)$)", std::regex::icase), AtAiMode::SharedFull);
      result.is_ai_invoke) {
    return result;
  }
  if (auto result = MatchPrompt(trimmed, std::regex(R"(^@ai\+\s+(.+)$)", std::regex::icase), AtAiMode::SharedReply);
      result.is_ai_invoke) {
    return result;
  }
  if (auto result =
          MatchPrompt(trimmed, std::regex(R"(^@ai\s+share\s+all\s+(.+)$)", std::regex::icase), AtAiMode::SharedFull);
      result.is_ai_invoke) {
    return result;
  }
  if (auto result =
          MatchPrompt(trimmed, std::regex(R"(^@ai\s+share\s+(.+)$)", std::regex::icase), AtAiMode::SharedReply);
      result.is_ai_invoke) {
    return result;
  }
  if (auto result = MatchPrompt(trimmed, std::regex(R"(^@ai\s+(.+)$)", std::regex::icase), AtAiMode::Local);
      result.is_ai_invoke) {
    return result;
  }
  return {};
}

} // namespace pbr
