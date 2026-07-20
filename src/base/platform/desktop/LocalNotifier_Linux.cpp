#if !defined(_WIN32) && !defined(__APPLE__)

#include "base/platform/desktop/LocalNotifierImpl.h"

#include "base/platform/ProductBranding.h"

#include <cstdlib>
#include <string>

namespace pbr::desktop {

namespace {

std::string ShellEscapeSingleQuotes(const std::string& input) {
  std::string out;
  out.reserve(input.size() + 8);
  for (char c : input) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  return out;
}

} // namespace

void PostDesktopNotification(const std::string& title, const std::string& body) {
  const std::string cmd = "notify-send --app-name=" + std::string(kProductName) + " '" +
                          ShellEscapeSingleQuotes(title) + "' '" +
                          ShellEscapeSingleQuotes(body) + "' >/dev/null 2>&1 &";
  (void)std::system(cmd.c_str());
}

} // namespace pbr::desktop

#endif
