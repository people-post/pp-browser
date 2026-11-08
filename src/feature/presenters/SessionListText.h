#pragma once

#include "common/CivilTime.h"
#include "foundation/i18n/LocalizationService.h"
#include "lib/messaging/AiThreadTitle.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>
#include <utility>

namespace pbr {

/** Pure text rules for a row in the sessions list (no RmlUi). */

constexpr size_t kSessionPreviewMaxChars = 120;

/**
 * The last message as one line for a row in the sessions list. An AI answer's preview is stored as
 * rendered markup, so tags are dropped and entities decoded (a `<` that does not open a tag is kept:
 * people type "1 < 2"). Markdown links show their text, bold markers are dropped, whitespace is collapsed,
 * and the line is cut to `max_chars` characters (UTF-8 aware) with an ellipsis. The view still truncates
 * to the row's width; the cut only bounds the string.
 */
inline std::string SessionPreviewLine(std::string_view raw, const size_t max_chars = kSessionPreviewMaxChars) {
  std::string text;
  text.reserve(raw.size());
  for (size_t i = 0; i < raw.size();) {
    if (raw[i] == '<' && i + 1 < raw.size()) {
      const char next = raw[i + 1] == '/' && i + 2 < raw.size() ? raw[i + 2] : raw[i + 1];
      const bool tag_like = (next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z');
      const size_t close = tag_like ? raw.find('>', i + 1) : std::string_view::npos;
      if (close != std::string_view::npos) {
        text += ' '; // a tag separates the text around it
        i = close + 1;
        continue;
      }
    }
    if (raw[i] == '&') {
      static constexpr std::pair<std::string_view, char> kEntities[] = {
          {"&lt;", '<'}, {"&gt;", '>'}, {"&amp;", '&'}, {"&quot;", '"'}, {"&#39;", '\''}, {"&nbsp;", ' '}};
      bool decoded = false;
      for (const auto& [entity, ch] : kEntities) {
        if (raw.substr(i, entity.size()) == entity) {
          text += ch;
          i += entity.size();
          decoded = true;
          break;
        }
      }
      if (decoded) {
        continue;
      }
    }
    text += raw[i++];
  }

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

/** A row's second line: the preview, or the placeholder when the thread has no text yet (so the row keeps its height). */
struct SessionPreviewShown {
  std::string text;
  /** `text` is the placeholder: the row draws it quieter and a search ignores it. */
  bool placeholder = false;
};

inline SessionPreviewShown ResolveSessionPreview(std::string line, std::string placeholder) {
  if (line.empty()) {
    return {std::move(placeholder), true};
  }
  return {std::move(line), false};
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
 * When a session was last active, for the list row: the time today ("14:05"), "Yesterday", the weekday name for
 * 2-6 days ago ("Wednesday"), the date earlier this year ("10/3"), the full date for earlier years ("2025/12/31").
 * Empty for an unset timestamp.
 */
inline std::string SessionDateLabel(const std::tm& when, const std::tm& now) {
  char buf[64];
  // Whole days between the two dates, and the weekday, from noon copies (tm_wday / tm_yday of the inputs may be unset).
  auto noon = [](std::tm t) {
    t.tm_hour = 12;
    t.tm_min = 0;
    t.tm_sec = 0;
    t.tm_isdst = -1;
    const std::time_t secs = std::mktime(&t);
    return std::pair<std::time_t, int>(secs, t.tm_wday);
  };
  const auto [when_noon, when_wday] = noon(when);
  const auto [now_noon, now_wday] = noon(now);
  (void)now_wday;
  const long days = static_cast<long>((now_noon - when_noon + 43200) / 86400);
  if (when.tm_year == now.tm_year && when.tm_yday == now.tm_yday) {
    std::snprintf(buf, sizeof(buf), "%02d:%02d", when.tm_hour, when.tm_min);
  } else if (days == 1) {
    return Tr("chat.date.yesterday");
  } else if (days >= 2 && days <= 6) {
    return Tr("home.weekday." + std::to_string(when_wday));
  } else if (when.tm_year == now.tm_year) {
    std::snprintf(buf, sizeof(buf), "%d/%d", when.tm_mon + 1, when.tm_mday);
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
