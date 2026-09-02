#pragma once

#include <cstdint>
#include <vector>

namespace pbr {

/** u64-BE length + body (Amp L4 and legacy stream framing share this shape). */
std::vector<uint8_t> EncodeLengthPrefixedFrame(const std::vector<uint8_t>& body);
uint64_t DecodeLengthPrefixedHeader(const std::vector<uint8_t>& header8);

} // namespace pbr
