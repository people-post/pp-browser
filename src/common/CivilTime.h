#pragma once

#include <ctime>

namespace pbr::civil_time {

/** UTC calendar time from a broken-down `tm` (`timegm` / `_mkgmtime`). */
time_t TimeGm(std::tm* tm);

/** Local broken-down time (`localtime_r` / `localtime_s`). */
bool LocalTime(time_t time, std::tm* out);

/** UTC broken-down time (`gmtime_r` / `gmtime_s`). */
bool UtcTime(time_t time, std::tm* out);

} // namespace pbr::civil_time
