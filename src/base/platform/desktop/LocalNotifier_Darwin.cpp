#if defined(__APPLE__)

#include "base/platform/desktop/LocalNotifierImpl.h"

#include <TargetConditionals.h>

#if !TARGET_OS_IPHONE

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
  const std::string cmd = "osascript -e 'display notification \"" + ShellEscapeSingleQuotes(body) +
                          "\" with title \"" + ShellEscapeSingleQuotes(title) + "\"' >/dev/null 2>&1 &";
  (void)std::system(cmd.c_str());
}

} // namespace pbr::desktop

#else

namespace pbr::desktop {

void PostDesktopNotification(const std::string& /*title*/, const std::string& /*body*/) {}

} // namespace pbr::desktop

#endif

#endif
