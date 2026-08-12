#pragma once

#include "base/crypto/CryptoTypes.h"

#include "common/Error.h"

#include <cstddef>
#include <string>

namespace pbr {

/** ML-KEM-768 (FIPS 203) via vendored mlkem-native — public-tier auto-key (PQ-only). */
inline constexpr size_t kHybridKemPublicKeyBytes = 1184;
inline constexpr size_t kHybridKemCiphertextBytes = 1088;
inline constexpr size_t kHybridKemSharedSecretBytes = 32;
inline constexpr size_t kHybridKemPrivateKeyBytes = 2400;

struct HybridKemKeyPair {
  ByteVector public_key;
  ByteVector private_key;
};

/**
 * ML-KEM-768 encaps/decaps for `e2e_public` key_init.
 * Class name kept for call-site stability; no longer X25519+Kyber draft hybrid.
 */
class HybridKem {
public:
  static Roe<HybridKemKeyPair> GenerateKeyPair();
  static Roe<ByteVector> Encapsulate(const ByteVector& peer_public_key, std::string& key_init_b64_out);
  static Roe<ByteVector> Decapsulate(const ByteVector& private_key, const std::string& key_init_b64);
};

} // namespace pbr
