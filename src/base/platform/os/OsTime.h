#pragma once

#include <ctime>

namespace pbr::os {

time_t TimeGm(std::tm* tm);
bool LocalTime(time_t time, std::tm* out);

} // namespace pbr::os
