#pragma once

#include <cstddef>
#include <string>

namespace pbr {

/**
 * The text of the desktop window's custom title strip: `PP · <nickname>` once the account has a name, plain `PP` before
 * (the first-run page) and after a reset. `product` is the localised product name.
 */
inline std::string WindowTitleFor(const std::string& product, const std::string& nickname) {
  const std::string blanks = " \t\r\n";
  const size_t first = nickname.find_first_not_of(blanks);
  if (first == std::string::npos) {
    return product;
  }
  const size_t last = nickname.find_last_not_of(blanks);
  return product + " \xC2\xB7 " + nickname.substr(first, last - first + 1);
}

} // namespace pbr
