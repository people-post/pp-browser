#pragma once

#include "foundation/data/Config.h"
#include "foundation/platform/BackendTable.h"
#include "foundation/platform/Platform.h"

#include <string>

namespace pbr {

class PlatformDefaults {
public:
  /** The defaults for this run's backend (DefaultBackendTable(): a custom backend starts with no service). */
  static AppConfig For(PlatformKind kind);
  /** The defaults against backend `origin` (every service under it); "" leaves every backend endpoint empty. */
  static AppConfig For(PlatformKind kind, const std::string& origin);
  /** The defaults against `table`: each endpoint is the table's, an absent service stays empty (no fallback). */
  static AppConfig ForTable(const BackendTable& table);
};

} // namespace pbr
