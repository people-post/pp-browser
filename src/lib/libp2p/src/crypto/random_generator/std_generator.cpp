/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <libp2p/crypto/random_generator/std_generator.hpp>

namespace libp2p::crypto::random {

  uint8_t StdRandomGenerator::randomByte() {
    return static_cast<uint8_t>(distribution_(generator_));
  }

  std::vector<uint8_t> StdRandomGenerator::randomBytes(size_t len) {
    std::vector<uint8_t> buffer(len, 0);
    for (size_t i = 0; i < len; ++i) {
      buffer[i] = randomByte();
    }
    return buffer;
  }
}  // namespace libp2p::crypto::random
