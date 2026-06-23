#pragma once

#include <cstdint>
#include <string>

namespace pbr::util {

std::string GenerateUuid();
int64_t NowUnixMs();

} // namespace pbr::util