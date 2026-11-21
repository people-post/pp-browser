#pragma once

#include <string>
#include <vector>

namespace pbr {

class ViewCatalog {
public:
  static std::string ResolvePath(const std::string& key_or_path);
  /** Relative paths of every registered view key (tests: must match assets/views). */
  static std::vector<std::string> KnownViewPaths();
  /** The key a page is mounted under in the wide layout: its own `<key>_wide` view when it has one, else `key`
   *  (DEV/DESIGN-desktop-ui-split-rules.md: a page whose desktop layout differs gets its own view file). */
  static std::string KeyForLayout(const std::string& key, bool wide);
  /** Localized + include-expanded body; cached by resolved relative path. */
  static std::string LoadBody(const std::string& key_or_path);
  static std::string LoadFile(const std::string& absolute_path);
  /** Drop cached bodies (call on locale change before remount). */
  static void ClearCache();
};

} // namespace pbr
