#pragma once

#include "common/Error.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** Write whole-file replace: tmp in same directory → fsync → rename. */
class AtomicFileWrite {
public:
  static Roe<void> Write(const std::string& path, std::string_view data);
  static Roe<void> Write(const std::string& path, const std::vector<uint8_t>& data);
};

} // namespace pbr
