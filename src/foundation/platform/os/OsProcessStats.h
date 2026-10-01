#pragma once

#include <cstdint>
#include "common/PbrCompat.h"

namespace pbr {

/** This process's resource use (operator metrics). `available` is false where the OS backend has none. */
struct OsProcessStats {
  bool available = false;
  /** User + system CPU time since start. */
  double cpu_seconds = 0.0;
  int64_t resident_bytes = 0;
  int64_t threads = 0;
  int64_t open_fds = 0;
};

OsProcessStats ReadOsProcessStats();

} // namespace pbr
