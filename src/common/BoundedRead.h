#pragma once

#include "common/Error.h"

#include <cstddef>
#include <cstdint>
#include <istream>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The rest of an open stream, refused when it holds more than `max_bytes` (PB-063): a file grown huge
 * — a bug, a full disk's leftovers, someone with access to the data dir — costs at most `max_bytes`
 * of memory, not all of it. Reads at most `max_bytes + 1` bytes; the caller opens (and so tells a
 * missing file from a large one).
 */
Roe<std::string> ReadBounded(std::istream& in, size_t max_bytes);
Roe<std::vector<uint8_t>> ReadBoundedBytes(std::istream& in, size_t max_bytes);

} // namespace pbr
