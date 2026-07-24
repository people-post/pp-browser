#pragma once

#include <cstdint>
#include <string>

namespace pbr::util {

std::string GenerateUuid();
int64_t NowUnixMs();

/** Strip leading/trailing ASCII whitespace (isspace). */
std::string Trim(const std::string& text);

/** ASCII lowercase (A–Z only). */
std::string ToLowerAscii(std::string text);

} // namespace pbr::util
