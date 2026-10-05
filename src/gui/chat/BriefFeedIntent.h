#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace pbr {

/**
 * "Give me today's briefs": a typed request for the Brief feed, which the app shows itself (the same
 * function as the Home "Latest briefs" chip) instead of asking the AI, where it turned into a web search.
 * "简报" / "briefs" is the agreed word for that feed. A request to write, explain or translate a briefing,
 * or a briefing about some topic, is not this and goes to the AI as usual.
 */
struct BriefFeedRequest {
  bool matched = false;
  bool chinese = false; // the sentence is Chinese: show the Chinese feed whatever the UI language
};

inline BriefFeedRequest MatchBriefFeedRequest(const std::string_view text) {
  constexpr size_t kMaxBytes = 120; // a short ask, not a paragraph that happens to mention the word
  if (text.empty() || text.size() > kMaxBytes) {
    return {};
  }
  const auto has = [&text](const std::string_view needle) { return text.find(needle) != std::string_view::npos; };

  if (has("\xE7\xAE\x80\xE6\x8A\xA5")) { // 简报
    // 写 起草 生成 翻译 总结 关于 是什么 什么是 怎么 如何 为什么 做一份 做个
    constexpr std::array<std::string_view, 13> kNot = {
        "\xE5\x86\x99",             "\xE8\xB5\xB7\xE8\x8D\x89", "\xE7\x94\x9F\xE6\x88\x90",             "\xE7\xBF\xBB\xE8\xAF\x91",
        "\xE6\x80\xBB\xE7\xBB\x93", "\xE5\x85\xB3\xE4\xBA\x8E", "\xE6\x98\xAF\xE4\xBB\x80\xE4\xB9\x88", "\xE4\xBB\x80\xE4\xB9\x88\xE6\x98\xAF",
        "\xE6\x80\x8E\xE4\xB9\x88", "\xE5\xA6\x82\xE4\xBD\x95", "\xE4\xB8\xBA\xE4\xBB\x80\xE4\xB9\x88", "\xE5\x81\x9A\xE4\xB8\x80\xE4\xBB\xBD",
        "\xE5\x81\x9A\xE4\xB8\xAA"};
    const bool other = std::any_of(kNot.begin(), kNot.end(), has);
    return BriefFeedRequest{!other, true};
  }

  // English: the plural / gerund noun only. "brief" alone is usually the adjective ("a brief summary").
  std::string lower(text);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
  const auto has_word = [&lower](const std::string_view word) {
    for (size_t pos = lower.find(word); pos != std::string::npos; pos = lower.find(word, pos + 1)) {
      const bool left = pos == 0 || std::isalpha(static_cast<unsigned char>(lower[pos - 1])) == 0;
      const size_t end = pos + word.size();
      const bool right = end == lower.size() || std::isalpha(static_cast<unsigned char>(lower[end])) == 0;
      if (left && right) {
        return true;
      }
    }
    return false;
  };
  if (!has_word("briefs") && !has_word("briefing") && !has_word("briefings")) {
    return {};
  }
  constexpr std::array<std::string_view, 11> kNot = {"write", "draft", "make",  "create", "translate", "summarize",
                                                     "about", "what",  "how",   "why",    "on"};
  const bool other = std::any_of(kNot.begin(), kNot.end(), has_word);
  return BriefFeedRequest{!other, false};
}

} // namespace pbr
