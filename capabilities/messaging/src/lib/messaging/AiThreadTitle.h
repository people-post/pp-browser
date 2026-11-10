#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace pbr {

/**
 * Title and privacy rules for an AI conversation named after the user's first message. One source of truth
 * for the stored title and the list preview (which also feed the Chats list and notification previews).
 */

inline constexpr size_t kAiThreadTitleMaxChars = 24;
inline constexpr std::string_view kDeviceLinkPrefix = "pp-browser-link-device-v1";

namespace ai_title_detail {

/** The codepoint at `s[i]` and the byte length of its encoding (1 for an invalid lead byte). */
inline uint32_t DecodeAt(std::string_view s, size_t i, size_t& len) {
  const auto b0 = static_cast<unsigned char>(s[i]);
  size_t n = b0 < 0x80 ? 1 : (b0 >> 5) == 0x6 ? 2 : (b0 >> 4) == 0xE ? 3 : (b0 >> 3) == 0x1E ? 4 : 1;
  if (i + n > s.size()) {
    n = 1;
  }
  len = n;
  if (n == 1) {
    return b0;
  }
  uint32_t cp = b0 & (0xFFu >> (n + 1));
  for (size_t k = 1; k < n; ++k) {
    cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3Fu);
  }
  return cp;
}

/** Codepoints that attach to the one before them (marks, variation selectors, skin tones, tags). */
inline bool IsExtender(uint32_t cp) {
  return (cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x1AB0 && cp <= 0x1AFF) || (cp >= 0x1DC0 && cp <= 0x1DFF) ||
         (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xFE20 && cp <= 0xFE2F) ||
         (cp >= 0x1F3FB && cp <= 0x1F3FF) || (cp >= 0xE0020 && cp <= 0xE007F) || (cp >= 0xE0100 && cp <= 0xE01EF) ||
         cp == 0x200C;
}

inline bool IsRegionalIndicator(uint32_t cp) {
  return cp >= 0x1F1E6 && cp <= 0x1F1FF;
}

/** Byte length of the display character (extended grapheme cluster, approximated) starting at `i`. */
inline size_t ClusterLength(std::string_view s, size_t i) {
  size_t len = 0;
  const uint32_t first = DecodeAt(s, i, len);
  size_t end = i + len;
  bool pair_open = IsRegionalIndicator(first); // a flag is two regional indicators
  while (end < s.size()) {
    size_t next_len = 0;
    const uint32_t cp = DecodeAt(s, end, next_len);
    if (IsExtender(cp)) {
      end += next_len;
    } else if (cp == 0x200D) { // zero-width joiner: the next codepoint joins this character
      end += next_len;
      if (end < s.size()) {
        DecodeAt(s, end, next_len);
        end += next_len;
      }
    } else if (pair_open && IsRegionalIndicator(cp)) {
      pair_open = false;
      end += next_len;
    } else {
      break;
    }
  }
  return end - i;
}

inline bool IsSpace(char c) {
  return c == ' ' || c == '\n' || c == '\r' || c == '\t';
}

inline bool IsBase64Char(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
}

} // namespace ai_title_detail

/**
 * Whether a first message should not appear in a list row or preview: a device-link transfer code, a base64-like
 * run (32+ ASCII characters containing a digit), or a long opaque ASCII paste (over 200 characters with no
 * whitespace). A plain number (a year, an amount) and ordinary prose — including long space-free CJK text — are
 * NOT masked; only pasted codes are.
 */
inline bool LooksSensitiveForAiTitle(std::string_view message) {
  using namespace ai_title_detail;
  size_t begin = 0;
  size_t end = message.size();
  while (begin < end && IsSpace(message[begin])) {
    ++begin;
  }
  while (end > begin && IsSpace(message[end - 1])) {
    --end;
  }
  const std::string_view text = message.substr(begin, end - begin);
  if (text.empty()) {
    return false;
  }
  if (text.substr(0, kDeviceLinkPrefix.size()) == kDeviceLinkPrefix) {
    return true;
  }
  bool any_space = false;
  bool has_non_ascii = false;
  size_t codepoints = 0;
  size_t run = 0;
  bool run_has_digit = false;
  bool long_base64 = false;
  for (size_t i = 0; i < text.size();) {
    size_t len = 0;
    DecodeAt(text, i, len);
    ++codepoints;
    const char c = text[i];
    any_space = any_space || IsSpace(c);
    has_non_ascii = has_non_ascii || (len > 1);
    if (len == 1 && IsBase64Char(c)) {
      ++run;
      run_has_digit = run_has_digit || (c >= '0' && c <= '9');
      long_base64 = long_base64 || (run >= 32 && run_has_digit);
    } else {
      run = 0;
      run_has_digit = false;
    }
    i += len;
  }
  // A long opaque ASCII paste (a transfer/recovery blob) has no spaces — but neither does CJK prose, so require the
  // blob to be ASCII and never flag a long Chinese message.
  return long_base64 || (!any_space && !has_non_ascii && codepoints > 200);
}

/**
 * Title for an AI thread from the user's first message: whitespace collapsed, cut to `kAiThreadTitleMaxChars`
 * display characters (never inside a CJK character, emoji sequence or flag) with an ellipsis. Empty when the
 * message has no text. Does not mask sensitive text; callers that store the result check LooksSensitiveForAiTitle.
 */
inline std::string AiThreadTitleFromMessage(std::string_view message) {
  using namespace ai_title_detail;
  std::string collapsed;
  bool pending_space = false;
  for (const char c : message) {
    if (IsSpace(c)) {
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
    i += ClusterLength(collapsed, i);
    ++chars;
  }
  if (i >= collapsed.size()) {
    return collapsed;
  }
  return collapsed.substr(0, i) + "…";
}

} // namespace pbr
