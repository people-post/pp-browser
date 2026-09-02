#include "domain/messaging/AtAiParser.h"

#include "common/Utilities.h"

#include <regex>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

AtAiParseResult MatchPrompt(const std::string& trimmed, const std::regex& pattern, const AtAiMode mode) {
  std::smatch match;
  if (std::regex_match(trimmed, match, pattern) && match.size() >= 2) {
    return AtAiParseResult{.is_ai_invoke = true, .mode = mode, .prompt = util::Trim(match[1].str())};
  }
  return {};
}

} // namespace

AtAiParseResult ParseAtAiPrefix(const std::string& text) {
  const std::string trimmed = util::Trim(text);
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
