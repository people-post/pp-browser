#include "foundation/diagnostics/OsCurrentMemory.h"

#include <unistd.h>

#include <cstdio>
#include <cstring>

namespace pbr {
namespace {

/** Pss in bytes from /proc/self/smaps_rollup; 0 when the file is unreadable (some Android builds refuse it). */
int64_t ReadPssBytes() {
  FILE* file = std::fopen("/proc/self/smaps_rollup", "r");
  if (!file) {
    return 0;
  }
  int64_t kib = 0;
  char line[256];
  while (std::fgets(line, sizeof line, file)) {
    if (std::strncmp(line, "Pss:", 4) == 0) {
      long long value = 0;
      if (std::sscanf(line + 4, " %lld", &value) == 1) {
        kib = value;
      }
      break;
    }
  }
  std::fclose(file);
  return kib * 1024;
}

/** Resident bytes from /proc/self/statm (second field, in pages); 0 when unreadable. */
int64_t ReadStatmRssBytes() {
  FILE* file = std::fopen("/proc/self/statm", "r");
  if (!file) {
    return 0;
  }
  long long size_pages = 0;
  long long resident_pages = 0;
  const int read = std::fscanf(file, "%lld %lld", &size_pages, &resident_pages);
  std::fclose(file);
  return read == 2 ? static_cast<int64_t>(resident_pages) * static_cast<int64_t>(sysconf(_SC_PAGESIZE)) : 0;
}

} // namespace

CurrentMemoryMb ReadCurrentMemoryMb() {
  const int64_t rss = ReadStatmRssBytes();
  const int64_t pss = ReadPssBytes();
  CurrentMemoryMb out;
  out.rss_mb = rss / 1'000'000;
  out.footprint_mb = (pss > 0 ? pss : rss) / 1'000'000; // no Pss: the RSS stands in
  return out;
}

} // namespace pbr
