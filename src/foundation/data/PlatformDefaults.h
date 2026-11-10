#pragma once

#include "foundation/data/Config.h"
#include "foundation/platform/Platform.h"

#include <string>

namespace pbr {

class PlatformDefaults {
public:
  /** The defaults for this run's backend origin (BriefOrigin()). */
  static AppConfig For(PlatformKind kind);
  /** The defaults against backend `origin`; "" leaves every backend endpoint empty. */
  static AppConfig For(PlatformKind kind, const std::string& origin);
};

} // namespace pbr
