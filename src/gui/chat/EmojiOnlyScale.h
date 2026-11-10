#pragma once

#include <cstdint>
#include <string_view>

namespace pbr {

/** How large a message that is only emoji is drawn (iOS style). */
enum class EmojiScale { None, Large, ExtraLarge };

namespace emoji_only_detail {

inline bool DecodeUtf8(std::string_view s, size_t& i, uint32_t& cp) {
  const auto c = static_cast<unsigned char>(s[i]);
  size_t len = 0;
  if (c < 0x80) {
    cp = c;
    len = 1;
  } else if ((c & 0xE0) == 0xC0) {
    cp = c & 0x1F;
    len = 2;
  } else if ((c & 0xF0) == 0xE0) {
    cp = c & 0x0F;
    len = 3;
  } else if ((c & 0xF8) == 0xF0) {
    cp = c & 0x07;
    len = 4;
  } else {
    return false;
  }
  if (i + len > s.size()) {
    return false;
  }
  for (size_t k = 1; k < len; ++k) {
    const auto cc = static_cast<unsigned char>(s[i + k]);
    if ((cc & 0xC0) != 0x80) {
      return false;
    }
    cp = (cp << 6) | (cc & 0x3F);
  }
  i += len;
  return true;
}

/** Symbols that draw as emoji even without U+FE0F. */
inline bool IsAlwaysEmojiSymbol(uint32_t cp) {
  switch (cp) {
    case 0x2764: // heart
    case 0x2728: // sparkles
    case 0x2B50: // star
    case 0x2600: // sun
    case 0x2601: // cloud
    case 0x260E: // telephone
    case 0x26A1: // high voltage
    case 0x26BD: // soccer ball
    case 0x26C4: // snowman
      return true;
    default:
      return false;
  }
}

/** Symbols that are text by default and emoji only with U+FE0F after them. */
inline bool IsTextDefaultSymbol(uint32_t cp) {
  return (cp >= 0x2600 && cp <= 0x27BF) || (cp >= 0x2B00 && cp <= 0x2BFF) || (cp >= 0x25AA && cp <= 0x25FE) ||
         (cp >= 0x2934 && cp <= 0x2935) || cp == 0x203C || cp == 0x2049 || cp == 0x2139 || cp == 0x3030 ||
         cp == 0x303D || cp == 0x3297 || cp == 0x3299;
}

/** `vs16` is true when U+FE0F follows. Arrows, technical symbols, (c) (r) and (tm) are not emoji. */
inline bool IsPictographic(uint32_t cp, bool vs16) {
  if (cp >= 0x1F000 && cp <= 0x1FAFF) {
    return true;
  }
  if (IsAlwaysEmojiSymbol(cp)) {
    return true;
  }
  return vs16 && IsTextDefaultSymbol(cp);
}

inline bool IsKeycapBase(uint32_t cp) {
  return (cp >= '0' && cp <= '9') || cp == '#' || cp == '*';
}

inline bool IsRegionalIndicator(uint32_t cp) {
  return cp >= 0x1F1E6 && cp <= 0x1F1FF;
}
inline bool IsSkinTone(uint32_t cp) {
  return cp >= 0x1F3FB && cp <= 0x1F3FF;
}
inline bool IsTag(uint32_t cp) {
  return cp >= 0xE0020 && cp <= 0xE007F;
}

} // namespace emoji_only_detail

/**
 * Decides how a text message is drawn: 1-3 emoji and nothing else -> ExtraLarge (about 3x, no bubble), 4-6 -> Large
 * (about 2x), anything else (a letter, a digit, punctuation, more than 6) -> None. Spaces between emoji are ignored.
 * Counts graphemes: a ZWJ family, a skin-toned hand, a flag (two regional indicators) or a variation-selector
 * symbol is one emoji.
 */
inline EmojiScale EmojiOnlyScale(std::string_view text) {
  using namespace emoji_only_detail;
  int count = 0;
  bool open = false;        // inside a cluster that can still take joiners / modifiers
  bool after_zwj = false;   // the next pictographic joins the previous cluster
  int regional_pending = 0; // 1 after a lone regional indicator
  size_t i = 0;
  while (i < text.size()) {
    uint32_t cp = 0;
    if (!DecodeUtf8(text, i, cp)) {
      return EmojiScale::None;
    }
    if (cp == ' ' || cp == '\n' || cp == '\t' || cp == 0x00A0) {
      open = false;
      after_zwj = false;
      regional_pending = 0;
      continue;
    }
    if (cp == 0xFE0F || cp == 0x20E3 || IsSkinTone(cp) || IsTag(cp)) {
      if (!open) {
        return EmojiScale::None;
      }
      continue;
    }
    if (cp == 0x200D) {
      if (!open) {
        return EmojiScale::None;
      }
      after_zwj = true;
      continue;
    }
    if (IsRegionalIndicator(cp)) {
      if (regional_pending == 1 && open) {
        regional_pending = 0;
        continue;
      }
      ++count;
      open = true;
      after_zwj = false;
      regional_pending = 1;
      continue;
    }
    size_t peek = i;
    uint32_t next = 0;
    const bool has_next = peek < text.size() && DecodeUtf8(text, peek, next);
    if (IsKeycapBase(cp) && has_next && (next == 0xFE0F || next == 0x20E3)) {
      // digit / # / * + FE0F + 20E3 is one keycap emoji; the FE0F and 20E3 that follow join it.
      size_t after = peek;
      uint32_t third = 0;
      const bool keycap = next == 0x20E3 || (after < text.size() && DecodeUtf8(text, after, third) && third == 0x20E3);
      if (keycap) {
        regional_pending = 0;
        ++count;
        open = true;
        after_zwj = false;
        continue;
      }
    }
    if (IsPictographic(cp, has_next && next == 0xFE0F)) {
      regional_pending = 0;
      if (!(after_zwj && open)) {
        ++count;
      }
      open = true;
      after_zwj = false;
      continue;
    }
    return EmojiScale::None;
  }
  if (count >= 1 && count <= 3) {
    return EmojiScale::ExtraLarge;
  }
  if (count >= 4 && count <= 6) {
    return EmojiScale::Large;
  }
  return EmojiScale::None;
}

} // namespace pbr
