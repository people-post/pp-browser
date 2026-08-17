/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <libp2p/crypto/mldsa_provider/mldsa_provider_impl.hpp>

#include <mldsa_native.h>

#include <libp2p/crypto/error.hpp>

namespace libp2p::crypto::mldsa {

  outcome::result<Keypair> MlDsaProviderImpl::generate() const {
    Keypair keypair;
    keypair.public_key.resize(kPublicKeyBytes);
    keypair.private_key.resize(kPrivateKeyBytes);
    if (mldsa_keypair(keypair.public_key.data(), keypair.private_key.data())
        != 0) {
      return KeyGeneratorError::KEY_GENERATION_FAILED;
    }
    return keypair;
  }

  outcome::result<Signature> MlDsaProviderImpl::sign(
      BytesIn message, const PrivateKey &private_key) const {
    if (private_key.size() != kPrivateKeyBytes) {
      return KeyGeneratorError::KEY_GENERATION_FAILED;
    }
    Signature signature(kSignatureBytes);
    if (mldsa_signature(signature.data(),
                        message.data(),
                        static_cast<size_t>(message.size()),
                        nullptr,
                        0,
                        private_key.data())
        != 0) {
      return CryptoProviderError::SIGNATURE_GENERATION_FAILED;
    }
    return signature;
  }

  outcome::result<bool> MlDsaProviderImpl::verify(
      BytesIn message,
      const Signature &signature,
      const PublicKey &public_key) const {
    if (public_key.size() != kPublicKeyBytes) {
      return false;
    }
    if (signature.size() != kSignatureBytes) {
      return false;
    }
    const int rc = mldsa_verify(signature.data(),
                                message.data(),
                                static_cast<size_t>(message.size()),
                                nullptr,
                                0,
                                public_key.data());
    return rc == 0;
  }

}  // namespace libp2p::crypto::mldsa
