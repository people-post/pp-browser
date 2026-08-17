/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <vector>

#include <libp2p/common/types.hpp>
#include <libp2p/outcome/outcome.hpp>

namespace libp2p::crypto::mldsa {

  /// FIPS 204 ML-DSA-65 sizes (mldsa-native).
  inline constexpr size_t kPublicKeyBytes = 1952u;
  inline constexpr size_t kPrivateKeyBytes = 4032u;
  inline constexpr size_t kSignatureBytes = 3309u;

  using PrivateKey = std::vector<uint8_t>;
  using PublicKey = std::vector<uint8_t>;
  using Signature = std::vector<uint8_t>;

  struct Keypair {
    PrivateKey private_key;
    PublicKey public_key;
  };

  class MlDsaProvider {
   public:
    virtual ~MlDsaProvider() = default;

    virtual outcome::result<Keypair> generate() const = 0;

    virtual outcome::result<Signature> sign(
        BytesIn message, const PrivateKey &private_key) const = 0;

    virtual outcome::result<bool> verify(BytesIn message,
                                         const Signature &signature,
                                         const PublicKey &public_key) const = 0;
  };

}  // namespace libp2p::crypto::mldsa
