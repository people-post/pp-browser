#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace pbr {

std::string EncodeBase58(std::span<const uint8_t> bytes);

} // namespace pbr
