/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <libp2p/crypto/mldsa_provider.hpp>

namespace libp2p::crypto::mldsa {

  class MlDsaProviderImpl : public MlDsaProvider {
   public:
    outcome::result<Keypair> generate() const override;

    outcome::result<Signature> sign(
        BytesIn message, const PrivateKey &private_key) const override;

    outcome::result<bool> verify(BytesIn message,
                                 const Signature &signature,
                                 const PublicKey &public_key) const override;
  };

}  // namespace libp2p::crypto::mldsa
