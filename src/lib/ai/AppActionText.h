#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace pbr::app_action_text {

/**
 * Internal to the on-device app-action code (AppActionClassifier.cpp, LocalActionPlanner.cpp): one UTF-8 decoder
 * and one word tokenizer, so the classifier's spans and the planner's offsets are measured the same way.
 */

/**
 * UTF-8 to code points; a malformed sequence becomes U+FFFD and consumes one byte. `offsets`, when given,
 * receives the byte offset of every code point in `in`.
 */
inline std::u32string DecodeUtf8(const std::string_view in, std::vector<size_t>* offsets = nullptr) {
  std::u32string out;
  for (size_t i = 0; i < in.size();) {
    if (offsets) {
      offsets->push_back(i);
    }
    const auto b = static_cast<unsigned char>(in[i]);
    const size_t n = b < 0x80 ? 1 : (b >> 5) == 0x6 ? 2 : (b >> 4) == 0xE ? 3 : (b >> 3) == 0x1E ? 4 : 0;
    char32_t cp = 0xFFFD;
    if (n > 0 && i + n <= in.size()) {
      cp = n == 1 ? b : b & (0xFFu >> (n + 1));
      for (size_t k = 1; k < n; ++k) {
        const auto c = static_cast<unsigned char>(in[i + k]);
        if ((c & 0xC0) != 0x80) {
          cp = 0xFFFD;
          break;
        }
        cp = (cp << 6) | (c & 0x3F);
      }
    }
    out.push_back(cp);
    i += n > 0 ? n : 1;
  }
  return out;
}

/** One English word: lowercase ASCII letters, digits and apostrophes; [begin, end) are bytes of the input. */
struct WordToken {
  std::string word;
  size_t begin = 0;
  size_t end = 0;
};

/** The words of `s` (ASCII lowercased; a curly apostrophe U+2019 counts as "'", so "I’d" is one word). */
inline std::vector<WordToken> TokenizeWords(const std::string_view s) {
  std::vector<WordToken> out;
  WordToken cur;
  const auto flush = [&](const size_t at) {
    if (!cur.word.empty()) {
      cur.end = at;
      out.push_back(std::move(cur));
      cur = WordToken{};
    }
  };
  for (size_t i = 0; i < s.size();) {
    char c = s[i];
    size_t n = 1;
    if (s.compare(i, 3, "\xE2\x80\x99") == 0) {
      c = '\'';
      n = 3;
    } else if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c + 32);
    }
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '\'') {
      if (cur.word.empty()) {
        cur.begin = i;
      }
      cur.word.push_back(c);
    } else {
      flush(i);
    }
    i += n;
  }
  flush(s.size());
  return out;
}

} // namespace pbr::app_action_text
