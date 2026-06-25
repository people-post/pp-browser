#pragma once

namespace pbr {

enum class PlatformKind { Desktop, Android, IOS };

class Platform {
public:
  static PlatformKind Detect();
  static bool IsMobile();
  static bool UsesPackagedAssets();
  static bool SupportsSubprocessMcp();
  static bool EarlyInit();
};
} // namespace pbr
