#pragma once

#include <cstdint>
#include <vector>

namespace pbr {

/** Protobuf wire encoding for libp2p PublicKey (ML-DSA-65, type code 4). */
std::vector<uint8_t> EncodeMlDsa65PublicKeyWire(const std::vector<uint8_t>& public_key);

} // namespace pbr
