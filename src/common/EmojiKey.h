#pragma once

#include <string>

namespace pbr {

/**
 * Normalize an emoji string for reaction equality (NFC-lite + strip trailing U+FE0F).
 * Does not rewrite the stored display glyph — use for keys / toggle matching only.
 */
inline std::string NormalizeEmojiKey(std::string emoji) {
  // Strip trailing variation selectors (emoji presentation).
  while (emoji.size() >= 3) {
    const unsigned char a = static_cast<unsigned char>(emoji[emoji.size() - 3]);
    const unsigned char b = static_cast<unsigned char>(emoji[emoji.size() - 2]);
    const unsigned char c = static_cast<unsigned char>(emoji[emoji.size() - 1]);
    // U+FE0F in UTF-8 is EF B8 8F; U+FE0E is EF B8 8E.
    if (a == 0xEF && b == 0xB8 && (c == 0x8F || c == 0x8E)) {
      emoji.resize(emoji.size() - 3);
      continue;
    }
    break;
  }
  // Trim ASCII whitespace that sometimes arrives from pickers.
  while (!emoji.empty() && (emoji.back() == ' ' || emoji.back() == '\t' || emoji.back() == '\n' ||
                            emoji.back() == '\r')) {
    emoji.pop_back();
  }
  size_t start = 0;
  while (start < emoji.size() &&
         (emoji[start] == ' ' || emoji[start] == '\t' || emoji[start] == '\n' || emoji[start] == '\r')) {
    ++start;
  }
  if (start > 0) {
    emoji.erase(0, start);
  }
  return emoji;
}

} // namespace pbr
