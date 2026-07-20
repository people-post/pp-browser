#if defined(_WIN32)

#include "base/platform/os/OsTime.h"

#include <time.h>

namespace pbr::os {

time_t TimeGm(std::tm* tm) {
  return _mkgmtime(tm);
}

bool LocalTime(time_t time, std::tm* out) {
  return localtime_s(out, &time) == 0;
}

} // namespace pbr::os

#endif
