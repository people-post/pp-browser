#pragma once

#include <string>
#include <string_view>

namespace pbr {

/** Lowercase hex SHA-256 of `data` (empty input is valid). */
std::string Sha256Hex(std::string_view data);

} // namespace pbr
