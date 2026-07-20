#if !defined(_WIN32)

#include "base/platform/os/OsTime.h"

#include <time.h>

namespace pbr::os {

time_t TimeGm(std::tm* tm) {
  return timegm(tm);
}

bool LocalTime(time_t time, std::tm* out) {
  return localtime_r(&time, out) != nullptr;
}

} // namespace pbr::os

#endif
