#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace pbr {

/**
 * Links the OS hands the app — a URL scheme it registered, opened from another app, a scanned QR
 * code or the command line. The app's handler takes them on the UI thread; links that come before
 * it is set (a cold start) wait for it. Thread-safe.
 */
class IncomingLink {
public:
  /** `scheme:...` with a scheme of two or more characters (not a path, not a drive letter). */
  static bool LooksLikeLink(std::string_view text);
  /** From any thread: to the handler on the UI thread, or kept until there is one. */
  static void Deliver(std::string link);
  /** Set (on the UI thread) once the app can act on links; empty to stop taking them. */
  static void SetHandler(std::function<void(std::string link)> handler);
};

} // namespace pbr
