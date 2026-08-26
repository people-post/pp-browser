#include "common/CivilTime.h"

#include <time.h>

namespace pp::civil_time {

time_t TimeGm(std::tm* tm) {
#if defined(_WIN32)
  return _mkgmtime(tm);
#else
  return timegm(tm);
#endif
}

bool LocalTime(time_t time, std::tm* out) {
  if (out == nullptr) {
    return false;
  }
#if defined(_WIN32)
  return localtime_s(out, &time) == 0;
#else
  return localtime_r(&time, out) != nullptr;
#endif
}

bool UtcTime(time_t time, std::tm* out) {
  if (out == nullptr) {
    return false;
  }
#if defined(_WIN32)
  return gmtime_s(out, &time) == 0;
#else
  return gmtime_r(&time, out) != nullptr;
#endif
}

} // namespace pp::civil_time
