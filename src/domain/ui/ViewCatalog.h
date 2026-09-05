#pragma once

#include <string>

namespace pbr {

class ViewCatalog {
public:
  static std::string ResolvePath(const std::string& key_or_path);
  /** Localized + include-expanded body; cached by resolved relative path. */
  static std::string LoadBody(const std::string& key_or_path);
  static std::string LoadFile(const std::string& absolute_path);
  /** Drop cached bodies (call on locale change before remount). */
  static void ClearCache();
};

} // namespace pbr
