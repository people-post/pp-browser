/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <libp2p/security/noise/crypto/noise_dh.hpp>

#include <mlkem_native.h>

#include <libp2p/crypto/error.hpp>

namespace libp2p::security::noise {
  namespace {
    constexpr size_t kPkBytes = 1184u;
    constexpr size_t kSkBytes = 2400u;
    constexpr size_t kCtBytes = 1088u;
    constexpr size_t kSsBytes = 32u;
  }  // namespace

  outcome::result<DHKey> NoiseDiffieHellmanImpl::generate() {
    DHKey key;
    key.pub.resize(kPkBytes);
    key.priv.resize(kSkBytes);
    if (mlkem_keypair(key.pub.data(), key.priv.data()) != 0) {
      return crypto::KeyGeneratorError::KEY_GENERATION_FAILED;
    }
    return key;
  }

  outcome::result<Bytes> NoiseDiffieHellmanImpl::dh(const Bytes &,
                                                    const Bytes &) {
    return crypto::KeyGeneratorError::UNSUPPORTED_KEY_TYPE;
  }

  outcome::result<KemEncapsulateResult> NoiseDiffieHellmanImpl::encapsulate(
      const Bytes &public_key) {
    if (public_key.size() != kPkBytes) {
      return crypto::KeyGeneratorError::KEY_GENERATION_FAILED;
    }
    KemEncapsulateResult out;
    out.ciphertext.resize(kCtBytes);
    out.shared_secret.resize(kSsBytes);
    if (mlkem_enc(out.ciphertext.data(),
                  out.shared_secret.data(),
                  public_key.data())
        != 0) {
      return crypto::KeyGeneratorError::KEY_GENERATION_FAILED;
    }
    return out;
  }

  outcome::result<Bytes> NoiseDiffieHellmanImpl::decapsulate(
      const Bytes &private_key, const Bytes &ciphertext) {
    if (private_key.size() != kSkBytes || ciphertext.size() != kCtBytes) {
      return crypto::KeyGeneratorError::KEY_GENERATION_FAILED;
    }
    Bytes shared(kSsBytes);
    if (mlkem_dec(shared.data(), ciphertext.data(), private_key.data()) != 0) {
      return crypto::KeyGeneratorError::KEY_GENERATION_FAILED;
    }
    return shared;
  }

  int NoiseDiffieHellmanImpl::dhSize() const {
    return static_cast<int>(kPkBytes);
  }

  int NoiseDiffieHellmanImpl::ciphertextSize() const {
    return static_cast<int>(kCtBytes);
  }

  std::string NoiseDiffieHellmanImpl::dhName() const {
    return "MLKEM768";
  }

}  // namespace libp2p::security::noise
