/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <random>

#include <libp2p/crypto/random_generator.hpp>

namespace libp2p::crypto::random {
  /**
   * CSPRNG backed by std::random_device (OS entropy).
   * Replaces the former Boost.Random-based generator.
   */
  class StdRandomGenerator : public CSPRNG {
   public:
    ~StdRandomGenerator() override = default;

    uint8_t randomByte() override;

    std::vector<uint8_t> randomBytes(size_t len) override;

   private:
    std::random_device generator_{};
    std::uniform_int_distribution<unsigned> distribution_{0, 255};
  };

  /// Compatibility alias used by older call sites / injectors.
  using BoostRandomGenerator = StdRandomGenerator;
}  // namespace libp2p::crypto::random
