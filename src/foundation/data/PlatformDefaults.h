#pragma once

#include "foundation/data/Config.h"
#include "foundation/platform/Platform.h"

namespace pbr {

class PlatformDefaults {
public:
  static AppConfig For(PlatformKind kind);
};

} // namespace pbr
