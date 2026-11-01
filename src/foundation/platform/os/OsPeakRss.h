#pragma once

#include <cstdint>

namespace pbr {

/**
 * The most resident memory this process has used since it started, in bytes (the OS's own high-water mark, so it
 * only ever grows); 0 where the OS gives none. For the memory numbers of large transfers.
 */
int64_t PeakResidentBytes();

} // namespace pbr
