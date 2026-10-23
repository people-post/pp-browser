#pragma once

#include <string>

namespace pbr {

/** Display string for nav unread badges (`0`, `1`…`99`, `99+`). */
inline std::string FormatBadgeCount(int count) {
  if (count <= 0) {
    return "0";
  }
  if (count > 99) {
    return "99+";
  }
  return std::to_string(count);
}

} // namespace pbr
