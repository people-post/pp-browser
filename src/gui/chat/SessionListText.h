#pragma once

#include "common/CivilTime.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>

namespace pbr {

/** Pure text rules for a row in the sessions list (no RmlUi). */

constexpr size_t kAiThreadTitleMaxChars = 24;

/**
 * Title for an AI thread from the user's first message: whitespace collapsed, cut to
 * `kAiThreadTitleMaxChars` characters (UTF-8 aware) with an ellipsis. Empty when the message has no text.
 */
inline std::string AiThreadTitleFromMessage(std::string_view message) {
  std::string collapsed;
  bool pending_space = false;
  for (const char c : message) {
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
      pending_space = !collapsed.empty();
      continue;
    }
    if (pending_space) {
      collapsed += ' ';
      pending_space = false;
    }
    collapsed += c;
  }
  size_t chars = 0;
  size_t i = 0;
  while (i < collapsed.size() && chars < kAiThreadTitleMaxChars) {
    ++i;
    while (i < collapsed.size() && (static_cast<unsigned char>(collapsed[i]) & 0xC0) == 0x80) {
      ++i; // continuation bytes belong to the character just counted
    }
    ++chars;
  }
  if (i >= collapsed.size()) {
    return collapsed;
  }
  return collapsed.substr(0, i) + "…";
}

constexpr size_t kSessionPreviewMaxChars = 120;

/**
 * The last message as one line for a row in the sessions list: Markdown links show their text, bold
 * markers are dropped, whitespace is collapsed, and the line is cut to `max_chars` characters (UTF-8
 * aware) with an ellipsis. The view still truncates to the row's width; the cut only bounds the string.
 */
inline std::string SessionPreviewLine(std::string_view text, const size_t max_chars = kSessionPreviewMaxChars) {
  std::string plain;
  plain.reserve(text.size());
  for (size_t i = 0; i < text.size();) {
    if (text[i] == '*' && i + 1 < text.size() && text[i + 1] == '*') {
      i += 2;
      continue;
    }
    if (text[i] == '[') {
      const size_t close = text.find(']', i + 1);
      if (close != std::string_view::npos && close + 1 < text.size() && text[close + 1] == '(') {
        const size_t end = text.find(')', close + 2);
        if (end != std::string_view::npos) {
          plain.append(text.substr(i + 1, close - i - 1));
          i = end + 1;
          continue;
        }
      }
    }
    plain += text[i++];
  }

  std::string line;
  bool pending_space = false;
  size_t chars = 0;
  for (size_t i = 0; i < plain.size();) {
    const char c = plain[i];
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
      pending_space = !line.empty();
      ++i;
      continue;
    }
    if (chars >= max_chars) {
      return line + "…";
    }
    if (pending_space) {
      line += ' ';
      pending_space = false;
    }
    line += c;
    ++i;
    while (i < plain.size() && (static_cast<unsigned char>(plain[i]) & 0xC0) == 0x80) {
      line += plain[i++]; // continuation bytes belong to the character just counted
    }
    ++chars;
  }
  return line;
}

/**
 * The AI thread a Home chip should continue instead of starting another one: the most recently updated
 * AI thread whose title is the chip's own (titles come from the first message, and a chip always sends
 * the same sentence). Empty when there is none. `Thread` needs `kind_is_ai`, `title`, `updated_at`, `id`
 * accessors through the callables, so this stays free of the thread types.
 */
template <typename Threads, typename IsAi, typename TitleOf, typename UpdatedAt, typename IdOf>
std::string FindChipThreadId(const Threads& threads, std::string_view chip_message, IsAi is_ai, TitleOf title_of,
                             UpdatedAt updated_at, IdOf id_of) {
  const std::string title = AiThreadTitleFromMessage(chip_message);
  std::string best_id;
  int64_t best_time = 0;
  if (title.empty()) {
    return best_id;
  }
  for (const auto& thread : threads) {
    if (is_ai(thread) && title_of(thread) == title && (best_id.empty() || updated_at(thread) > best_time)) {
      best_id = id_of(thread);
      best_time = updated_at(thread);
    }
  }
  return best_id;
}

/**
 * When a session was last active, for the list row: date and time within this year ("10/3 14:05"),
 * the full date for earlier years ("2025/12/31"). Empty for an unset timestamp.
 */
inline std::string SessionDateLabel(const std::tm& when, const std::tm& now) {
  char buf[64];
  if (when.tm_year == now.tm_year) {
    std::snprintf(buf, sizeof(buf), "%d/%d %02d:%02d", when.tm_mon + 1, when.tm_mday, when.tm_hour, when.tm_min);
  } else {
    std::snprintf(buf, sizeof(buf), "%d/%d/%d", when.tm_year + 1900, when.tm_mon + 1, when.tm_mday);
  }
  return buf;
}

inline std::string SessionDateLabel(const int64_t updated_at_ms, const int64_t now_ms) {
  if (updated_at_ms <= 0) {
    return {};
  }
  const std::time_t when_s = static_cast<std::time_t>(updated_at_ms / 1000);
  const std::time_t now_s = static_cast<std::time_t>(now_ms / 1000);
  std::tm when{};
  std::tm now{};
  if (!pp::civil_time::LocalTime(when_s, &when) || !pp::civil_time::LocalTime(now_s, &now)) {
    return {};
  }
  return SessionDateLabel(when, now);
}

} // namespace pbr
