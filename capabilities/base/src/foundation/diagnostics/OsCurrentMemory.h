#pragma once

#include <cstdint>

namespace pbr {

/** What this process holds right now, in megabytes (10^6 bytes, like peak_rss_mb); 0 where the OS gives none. */
struct CurrentMemoryMb {
  /** What the OS charges the process: Darwin phys_footprint, Linux / Android Pss, Windows the working set. */
  int64_t footprint_mb = 0;
  /** Resident set: Darwin resident_size, Linux / Android statm RSS, Windows the working set. */
  int64_t rss_mb = 0;
};

/** The current value (it also falls), unlike PeakResidentBytes. For the memory readings of chat.open / chat.window. */
CurrentMemoryMb ReadCurrentMemoryMb();

} // namespace pbr
