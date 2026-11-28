#pragma once

namespace pbr {

enum class PlatformKind { Desktop, Android, IOS };

class Platform {
public:
  static PlatformKind Detect();
  /** This build's OS: "ios", "android", "macos", "windows" or "linux" (client-compat `upgrade_urls` keys). */
  static const char* OsName();
  static bool IsMobile();
  static bool IsDesktop();
  static bool UsesPackagedAssets();
  static bool SupportsSubprocessMcp();
  static bool EarlyInit();
};
} // namespace pbr
