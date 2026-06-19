#pragma once

#include "app/Config.h"
#include "platform/Platform.h"

namespace pbr {

class PlatformDefaults {
public:
  static AppConfig For(PlatformKind kind);
};

} // namespace pbr
