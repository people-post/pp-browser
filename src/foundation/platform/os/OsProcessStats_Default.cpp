#include "foundation/platform/os/OsProcessStats.h"

#include "common/PbrCompat.h"

namespace pbr {

OsProcessStats ReadOsProcessStats() {
  return {};  // no backend yet (pp-node runs on Linux)
}

} // namespace pbr
