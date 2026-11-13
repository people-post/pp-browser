#include "foundation/diagnostics/OsCurrentMemory.h"

#include <windows.h>
#include <psapi.h>

namespace pbr {

CurrentMemoryMb ReadCurrentMemoryMb() {
  PROCESS_MEMORY_COUNTERS counters{};
  if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof counters)) {
    return {};
  }
  // No Pss on Windows: the working set is both numbers.
  const int64_t mb = static_cast<int64_t>(counters.WorkingSetSize / 1'000'000ULL);
  return {mb, mb};
}

} // namespace pbr
