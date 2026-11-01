#pragma once

#include <cstddef>
#include <string>

namespace pbr {

/** Longest nickname / contact remark, in Unicode code points. */
inline constexpr size_t kMaxNicknameChars = 32;

/** Cut a UTF-8 string after max_cp code points (never inside a character). */
inline std::string TruncateCodePoints(const std::string& s, const size_t max_cp) {
  size_t count = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
      if (count == max_cp) {
        return s.substr(0, i);
      }
      ++count;
    }
  }
  return s;
}

}  // namespace pbr
