#include "foundation/platform/os/OsProcessStats.h"

#include <dirent.h>
#include <unistd.h>

#include <fstream>
#include <sstream>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

/** utime + stime (fields 14 and 15 of /proc/self/stat, after the parenthesized command). */
double CpuSeconds() {
  std::ifstream stat("/proc/self/stat");
  std::string line;
  if (!std::getline(stat, line)) {
    return 0.0;
  }
  const auto close = line.rfind(')');
  if (close == std::string::npos) {
    return 0.0;
  }
  std::istringstream fields(line.substr(close + 2));
  std::string skip;
  for (int i = 3; i < 14; ++i) {  // state (3) … cmajflt (13)
    fields >> skip;
  }
  unsigned long long utime = 0;
  unsigned long long stime = 0;
  fields >> utime >> stime;
  const long ticks = sysconf(_SC_CLK_TCK);
  return ticks > 0 ? static_cast<double>(utime + stime) / static_cast<double>(ticks) : 0.0;
}

int64_t ResidentBytes() {
  std::ifstream statm("/proc/self/statm");
  long long size = 0;
  long long resident = 0;
  statm >> size >> resident;
  const long page = sysconf(_SC_PAGESIZE);
  return page > 0 ? static_cast<int64_t>(resident) * page : 0;
}

int64_t Threads() {
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.rfind("Threads:", 0) == 0) {
      return std::stoll(line.substr(8));
    }
  }
  return 0;
}

int64_t OpenFds() {
  DIR* dir = opendir("/proc/self/fd");
  if (!dir) {
    return 0;
  }
  int64_t count = 0;
  while (const dirent* entry = readdir(dir)) {
    if (entry->d_name[0] != '.') {
      ++count;
    }
  }
  closedir(dir);
  return count > 0 ? count - 1 : 0;  // the directory handle itself
}

} // namespace

OsProcessStats ReadOsProcessStats() {
  OsProcessStats stats;
  stats.available = true;
  stats.cpu_seconds = CpuSeconds();
  stats.resident_bytes = ResidentBytes();
  stats.threads = Threads();
  stats.open_fds = OpenFds();
  return stats;
}

} // namespace pbr
