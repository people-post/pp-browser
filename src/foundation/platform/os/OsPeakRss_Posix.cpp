#include "foundation/platform/os/OsPeakRss.h"

#include <sys/resource.h>

namespace pbr {

int64_t PeakResidentBytes() {
  struct rusage usage {};
  if (getrusage(RUSAGE_SELF, &usage) != 0) {
    return 0;
  }
#if defined(__APPLE__)
  return static_cast<int64_t>(usage.ru_maxrss); // bytes
#else
  return static_cast<int64_t>(usage.ru_maxrss) * 1024; // kilobytes
#endif
}

} // namespace pbr
