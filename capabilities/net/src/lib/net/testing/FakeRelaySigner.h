#pragma once

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/crypto/TypedBlob.h"
#include "lib/net/RelayAudience.h"

#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr::test {

/** The fill byte of the fake signature (`FakeRelaySigner`). */
inline constexpr uint8_t kFakeRelaySignatureFill = 0x5a;

/**
 * A relay signer for tests that do not verify signatures: a bare ML-DSA-65-sized signature of one fill byte, as the
 * key holders answer (`SignRelayCommand` sends it typed). Calls each signed body's bytes to `seen` when given.
 */
inline RelayAuthSigner FakeRelaySigner(std::vector<std::vector<uint8_t>>* seen = nullptr) {
  return [seen](const std::vector<uint8_t>& bytes) -> Roe<std::string> {
    if (seen != nullptr) {
      seen->push_back(bytes);
    }
    return Base64Encode(ByteVector(kMlDsa65SignatureBytes, kFakeRelaySignatureFill));
  };
}

/** The `signature` a body signed by `FakeRelaySigner` carries: the typed form of its signature. */
inline std::string FakeRelaySignatureB64() {
  return Base64Encode(TypedSignature(ByteVector(kMlDsa65SignatureBytes, kFakeRelaySignatureFill)).value());
}

} // namespace pbr::test
