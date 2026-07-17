#include "base/platform/DesktopLocalNotifier.h"

#include "base/platform/AppLifecycle.h"
#include "common/ProductBranding.h"

#include <cstdlib>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace pbr {

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

void PostDesktopNotification(const std::string& title, const std::string& body) {
#if defined(__linux__)
  const std::string cmd = "notify-send --app-name=" + std::string(kProductName) + " '" +
                          ShellEscapeSingleQuotes(title) + "' '" +
                          ShellEscapeSingleQuotes(body) + "' >/dev/null 2>&1 &";
  (void)std::system(cmd.c_str());
#elif defined(__APPLE__)
  const std::string cmd = "osascript -e 'display notification \"" + ShellEscapeSingleQuotes(body) +
                          "\" with title \"" + ShellEscapeSingleQuotes(title) + "\"' >/dev/null 2>&1 &";
  (void)std::system(cmd.c_str());
#elif defined(_WIN32)
  (void)title;
  (void)body;
  MessageBeep(MB_OK);
#else
  (void)title;
  (void)body;
#endif
}

} // namespace

void DesktopLocalNotifier::NotifyIncoming(const std::string& title, const std::string& body,
                                          const std::string& /*thread_id*/) {
  if (AppLifecycle::IsForeground()) {
    return;
  }
  PostDesktopNotification(title.empty() ? "New message" : title, body.empty() ? "You have a new message" : body);
}

void DesktopLocalNotifier::ClearForThread(const std::string& /*thread_id*/) {}

} // namespace pbr
