/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <libp2p/security/noise/crypto/noise_ccp1305.hpp>

namespace libp2p::security::noise {

  NoiseCCP1305Impl::NoiseCCP1305Impl(Key32 key)
      : ccp_{std::make_unique<crypto::chachapoly::ChaCha20Poly1305Impl>(key)} {}

  outcome::result<Bytes> NoiseCCP1305Impl::encrypt(BytesIn precompiled_out,
                                                   uint64_t nonce,
                                                   BytesIn plaintext,
                                                   BytesIn aad) {
    auto n = ccp_->uint64toNonce(nonce);
    auto enc_res = ccp_->encrypt(n, plaintext, aad);
    if (!enc_res) {
      return enc_res.error();
    }
    auto enc = std::move(enc_res).value();
    auto res = spanToVec(precompiled_out);
    res.reserve(res.size() + enc.size());
    res.insert(res.end(), enc.begin(), enc.end());
    return res;
  }

  outcome::result<Bytes> NoiseCCP1305Impl::decrypt(BytesIn precompiled_out,
                                                   uint64_t nonce,
                                                   BytesIn ciphertext,
                                                   BytesIn aad) {
    auto n = ccp_->uint64toNonce(nonce);
    auto dec_res = ccp_->decrypt(n, ciphertext, aad);
    if (!dec_res) {
      return dec_res.error();
    }
    auto dec = std::move(dec_res).value();
    auto res = spanToVec(precompiled_out);
    res.reserve(res.size() + dec.size());
    res.insert(res.end(), dec.begin(), dec.end());
    return res;
  }

  std::shared_ptr<AEADCipher> NamedCCPImpl::cipher(Key32 key) {
    return std::make_shared<NoiseCCP1305Impl>(key);
  }

  std::string NamedCCPImpl::cipherName() const {
    return "ChaChaPoly";
  }
}  // namespace libp2p::security::noise
