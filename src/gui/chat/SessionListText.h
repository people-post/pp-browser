#pragma once

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

/**
 * When a session was last active, for the list row: date and time within this year ("10/3 14:05"),
 * the full date for earlier years ("2025/12/31"). Empty for an unset timestamp.
 */
inline std::string SessionDateLabel(const std::tm& when, const std::tm& now) {
  char buf[24];
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
  localtime_r(&when_s, &when);
  localtime_r(&now_s, &now);
  return SessionDateLabel(when, now);
}

} // namespace pbr
