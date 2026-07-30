#pragma once

#include "common/Error.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pbr {

constexpr size_t kMaxStreamJsonFrameBytes = 64 * 1024;

/** u64-BE length prefix + UTF-8 JSON payload (shared by dial-back, circuit-relay, etc.). */
Roe<std::vector<uint8_t>> EncodeStreamJsonFrame(const std::string& json_utf8);
Roe<std::string> DecodeStreamJsonFrame(const std::vector<uint8_t>& frame_bytes);

} // namespace pbr
