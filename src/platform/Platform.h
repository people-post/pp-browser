#pragma once

namespace pbr {

enum class PlatformKind { Desktop, Android, IOS };

class Platform {
public:
  static PlatformKind Detect();
};

} // namespace pbr
