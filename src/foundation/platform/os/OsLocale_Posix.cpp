#include "foundation/platform/os/OsLocale.h"

#if !defined(_WIN32)

#include <cstdlib>
#include <string>

namespace pbr::os {

namespace {

// Strip encoding / modifier: en_US.UTF-8@euro → en_US
std::string StripLocaleEncoding(std::string tag) {
  const auto dot = tag.find('.');
  if (dot != std::string::npos) {
    tag.resize(dot);
  }
  const auto at = tag.find('@');
  if (at != std::string::npos) {
    tag.resize(at);
  }
  return tag;
}

} // namespace

std::vector<std::string> PreferredSystemLocales() {
  std::vector<std::string> out;
  for (const char* key : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
    const char* value = std::getenv(key);
    if (value == nullptr || value[0] == '\0') {
      continue;
    }
    std::string tag = StripLocaleEncoding(value);
    if (tag == "C" || tag == "POSIX") {
      continue;
    }
    out.push_back(std::move(tag));
    break;
  }
  return out;
}

} // namespace pbr::os

#endif // !_WIN32
