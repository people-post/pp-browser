#pragma once

#include "base/data/Config.h"
#include "base/platform/Platform.h"

namespace pbr {

class PlatformDefaults {
public:
  static AppConfig For(PlatformKind kind);
};

} // namespace pbr
