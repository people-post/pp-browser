/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <libp2p/crypto/hmac_provider/hmac_provider_ctr_impl.hpp>
#include <libp2p/crypto/hmac_provider/hmac_provider_impl.hpp>

#include <span>

namespace libp2p::crypto::hmac {

  outcome::result<Bytes> HmacProviderImpl::calculateDigest(
      HashType hash_type, const Bytes &key, BytesIn message) const {
    HmacProviderCtrImpl hmac{hash_type, key};
    auto write_res = hmac.write(message);
    if (not write_res) {
      return std::move(write_res).as_failure();
    }
    return hmac.digest();
  }

}  // namespace libp2p::crypto::hmac
