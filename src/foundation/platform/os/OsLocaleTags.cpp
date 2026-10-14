#include "foundation/platform/os/OsLocale.h"

#include <string>
#include <string_view>
#include <vector>

namespace pbr::os {

std::vector<std::string> SplitLanguageTags(std::string_view csv) {
  std::vector<std::string> out;
  while (!csv.empty()) {
    const size_t comma = csv.find(',');
    std::string_view tag = csv.substr(0, comma);
    while (!tag.empty() && (tag.front() == ' ' || tag.front() == '\t')) {
      tag.remove_prefix(1);
    }
    while (!tag.empty() && (tag.back() == ' ' || tag.back() == '\t')) {
      tag.remove_suffix(1);
    }
    if (!tag.empty()) {
      out.emplace_back(tag);
    }
    if (comma == std::string_view::npos) {
      break;
    }
    csv.remove_prefix(comma + 1);
  }
  return out;
}

} // namespace pbr::os
