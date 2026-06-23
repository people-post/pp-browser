/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <libp2p/crypto/hmac_provider/hmac_provider_ctr_impl.hpp>
#include <libp2p/security/noise/crypto/interfaces.hpp>

OUTCOME_CPP_DEFINE_CATEGORY(libp2p::security::noise, HKDFError, e) {
  using E = libp2p::security::noise::HKDFError;
  switch (e) {
    case E::ILLEGAL_OUTPUTS_NUMBER:
      return "Noise HKDF() may produce one, two, or three outputs only";
  }
  return "unknown";
}

namespace libp2p::security::noise {

  using HMAC = crypto::hmac::HmacProviderCtrImpl;

  outcome::result<HKDFResult> hkdf(HashType hash_type,
                                   size_t outputs,
                                   BytesIn chaining_key,
                                   BytesIn input_key_material) {
    if (0 == outputs or outputs > 3) {
      return HKDFError::ILLEGAL_OUTPUTS_NUMBER;
    }
    HKDFResult result;

    HMAC temp_mac{hash_type, spanToVec(chaining_key)};
    auto temp_mac_write_res = temp_mac.write(input_key_material);
    if (!temp_mac_write_res) {
      return temp_mac_write_res.error();
    }
    auto temp_key_res = temp_mac.digest();
    if (!temp_key_res) {
      return temp_key_res.error();
    }
    auto temp_key = std::move(temp_key_res).value();

    HMAC out1_mac{hash_type, temp_key};
    Bytes one(1, 0x01);
    auto out1_mac_write_res = out1_mac.write(one);
    if (!out1_mac_write_res) {
      return out1_mac_write_res.error();
    }
    auto out1_res = out1_mac.digest();
    if (!out1_res) {
      return out1_res.error();
    }
    auto out1 = std::move(out1_res).value();
    result.one = out1;
    if (1 == outputs) {
      return result;
    }

    HMAC out2_mac{hash_type, temp_key};
    auto out2_mac_write_out1_res = out2_mac.write(out1);
    if (!out2_mac_write_out1_res) {
      return out2_mac_write_out1_res.error();
    }
    Bytes two(1, 0x02);
    auto out2_mac_write_two_res = out2_mac.write(two);
    if (!out2_mac_write_two_res) {
      return out2_mac_write_two_res.error();
    }
    auto out2_res = out2_mac.digest();
    if (!out2_res) {
      return out2_res.error();
    }
    auto out2 = std::move(out2_res).value();
    result.two = out2;
    if (2 == outputs) {
      return result;
    }

    HMAC out3_mac{hash_type, temp_key};
    auto out3_mac_write_out2_res = out3_mac.write(out2);
    if (!out3_mac_write_out2_res) {
      return out3_mac_write_out2_res.error();
    }
    Bytes three(1, 0x03);
    auto out3_mac_write_three_res = out3_mac.write(three);
    if (!out3_mac_write_three_res) {
      return out3_mac_write_three_res.error();
    }
    auto out3_res = out3_mac.digest();
    if (!out3_res) {
      return out3_res.error();
    }
    auto out3 = std::move(out3_res).value();
    result.three = out3;

    return result;
  }

}  // namespace libp2p::security::noise
