#pragma once

#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace pbr {

/**
 * "Give me today's briefs": a typed request for the Brief feed, which the app shows itself (the same
 * function as the Home "Latest briefs" chip) instead of asking the AI, where it turned into a web search.
 * "简报" / "briefs" is the agreed word for that feed.
 *
 * The match is an allowlist on the shape of the sentence: besides the feed word, only request and time
 * fillers may appear ("给我 / 看看 / 今天 / 最新 / 有什么 … 吗", "show me the latest …"). Anything else
 * goes to the AI as usual: a question about a brief on screen ("第二条简报讲了什么"), a briefing to write
 * or about a topic, or another sense of the English word ("men's briefs", "legal briefs").
 */
struct BriefFeedRequest {
  bool matched = false;
  bool chinese = false; // the sentence is Chinese; otherwise it is English
};

namespace brief_feed_detail {

/** True when nothing is left of `text` after removing every `filler` (longest first) and non-word bytes. */
template <size_t N>
bool OnlyFillers(std::string text, const std::array<std::string_view, N>& fillers, const bool ascii_words) {
  for (const std::string_view filler : fillers) {
    for (size_t pos = text.find(filler); pos != std::string::npos; pos = text.find(filler, pos)) {
      const size_t end = pos + filler.size();
      // An English filler must be a whole word ("the" is not removed from "other").
      const bool whole = !ascii_words || ((pos == 0 || std::isalpha(static_cast<unsigned char>(text[pos - 1])) == 0) &&
                                          (end == text.size() || std::isalpha(static_cast<unsigned char>(text[end])) == 0));
      if (whole) {
        text.replace(pos, filler.size(), " ");
        ++pos;
      } else {
        pos = end;
      }
    }
  }
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte >= 0x80 || std::isalnum(byte) != 0) {
      return false; // a CJK byte, a letter or a digit is left
    }
  }
  return true;
}

} // namespace brief_feed_detail

inline BriefFeedRequest MatchBriefFeedRequest(const std::string_view text) {
  constexpr size_t kMaxBytes = 120; // a short ask, not a paragraph that happens to mention the word
  if (text.empty() || text.size() > kMaxBytes) {
    return {};
  }

  constexpr std::string_view kWordZh = "\xE7\xAE\x80\xE6\x8A\xA5"; // 简报
  if (text.find(kWordZh) != std::string_view::npos) {
    // Longest first. 我想看 我要看 有什么 有哪些 有没有 帮我 给我 看看 看一下 一下 今天 今日 现在 最新 最近
    // 新的 的 新 看 来 请 有 吗 呢 吧 啊 和 full-width punctuation ？ ！ 。 ，
    constexpr std::array<std::string_view, 30> kFillers = {
        kWordZh,
        "\xE6\x88\x91\xE6\x83\xB3\xE7\x9C\x8B", "\xE6\x88\x91\xE8\xA6\x81\xE7\x9C\x8B", "\xE6\x9C\x89\xE4\xBB\x80\xE4\xB9\x88",
        "\xE6\x9C\x89\xE5\x93\xAA\xE4\xBA\x9B", "\xE6\x9C\x89\xE6\xB2\xA1\xE6\x9C\x89", "\xE7\x9C\x8B\xE4\xB8\x80\xE4\xB8\x8B",
        "\xE5\xB8\xAE\xE6\x88\x91",             "\xE7\xBB\x99\xE6\x88\x91",             "\xE7\x9C\x8B\xE7\x9C\x8B",
        "\xE4\xB8\x80\xE4\xB8\x8B",             "\xE4\xBB\x8A\xE5\xA4\xA9",             "\xE4\xBB\x8A\xE6\x97\xA5",
        "\xE7\x8E\xB0\xE5\x9C\xA8",             "\xE6\x9C\x80\xE6\x96\xB0",             "\xE6\x9C\x80\xE8\xBF\x91",
        "\xE6\x96\xB0\xE7\x9A\x84",             "\xE7\x9A\x84",                         "\xE6\x96\xB0",
        "\xE7\x9C\x8B",                         "\xE6\x9D\xA5",                         "\xE8\xAF\xB7",
        "\xE6\x9C\x89",                         "\xE5\x90\x97",                         "\xE5\x91\xA2",
        "\xE5\x90\xA7",                         "\xEF\xBC\x9F",                         "\xEF\xBC\x81",
        "\xE3\x80\x82",                         "\xEF\xBC\x8C"};
    return BriefFeedRequest{brief_feed_detail::OnlyFillers(std::string(text), kFillers, false), true};
  }

  // English: the plural / gerund noun only ("brief" alone is usually the adjective), and nothing but fillers.
  std::string lower(text);
  for (char& c : lower) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  constexpr std::array<std::string_view, 26> kFillers = {"briefings", "briefing", "briefs", "today's", "todays", "please", "latest",
                                                         "newest",    "recent",   "today",  "there",   "could",  "show",   "give",
                                                         "want",      "the",      "new",    "any",     "get",    "see",    "can",
                                                         "you",       "are",      "me",     "to",      "i"};
  const bool has_word = lower.find("briefs") != std::string::npos || lower.find("briefing") != std::string::npos;
  return BriefFeedRequest{has_word && brief_feed_detail::OnlyFillers(lower, kFillers, true), false};
}

} // namespace pbr
