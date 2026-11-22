#pragma once

// Typed blobs of the right shape for tests that build an envelope by hand (envelope v4, release-compatibility
// SIGNED_FORMATS § 2.1): the receiver reads every typed blob's algorithm id and size before it verifies, so a fake
// signature, key_init or payload must be one — of the right algorithm and size, whatever its bytes.

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/HybridKem.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/crypto/TypedBlob.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace pbr::test {

/** A typed ML-DSA-65 signature of `fill` bytes (it verifies nothing). */
inline std::string FakeSignatureB64(const uint8_t fill = 0x5a) {
  return Base64Encode(TypedSignature(ByteVector(kMlDsa65SignatureBytes, fill)).value());
}

/** A typed ML-KEM-768 ciphertext of `fill` bytes (it opens nothing). */
inline std::string FakeKeyInitB64(const uint8_t fill = 0x4b) {
  return Base64Encode(TypedKemCiphertext(ByteVector(kHybridKemCiphertextBytes, fill)).value());
}

/** An AEAD payload blob (id 1, a nonce, a tag and `body_bytes` of ciphertext) of `fill` bytes: it decrypts to nothing. */
inline std::string FakePayloadB64(const uint8_t fill = 0x50, const size_t body_bytes = 8) {
  return Base64Encode(WriteTypedBlob(pp::BlobKind::kAeadSealed, ByteVector(24 + 16 + body_bytes, fill)).value());
}

/** An AEAD payload blob whose "ciphertext" is `marker`'s bytes (a zero nonce and tag): one per message, opening nothing. */
inline std::string FakePayloadOfB64(const std::string& marker) {
  ByteVector body(24 + 16, 0);
  body.insert(body.end(), marker.begin(), marker.end());
  return Base64Encode(WriteTypedBlob(pp::BlobKind::kAeadSealed, body).value());
}

} // namespace pbr::test
