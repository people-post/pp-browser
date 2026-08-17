/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <libp2p/security/noise/crypto/interfaces.hpp>

namespace libp2p::security::noise {

  /// ML-KEM-768 Noise DH/KEM (libp2p-pq-transport P002).
  class NoiseDiffieHellmanImpl : public DiffieHellman {
   public:
    outcome::result<DHKey> generate() override;

    outcome::result<Bytes> dh(const Bytes &private_key,
                              const Bytes &public_key) override;

    outcome::result<KemEncapsulateResult> encapsulate(
        const Bytes &public_key) override;

    outcome::result<Bytes> decapsulate(const Bytes &private_key,
                                       const Bytes &ciphertext) override;

    int dhSize() const override;

    int ciphertextSize() const override;

    std::string dhName() const override;
  };

}  // namespace libp2p::security::noise
