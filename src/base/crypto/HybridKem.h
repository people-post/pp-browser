#pragma once

#include "base/crypto/CryptoTypes.h"

#include "common/Error.h"

#include <cstddef>
#include <string>

namespace pbr {

inline constexpr size_t kHybridKemX25519PublicBytes = 32;
inline constexpr size_t kHybridKemPublicKeyBytes = 1216;
inline constexpr size_t kHybridKemCiphertextBytes = 1120;
inline constexpr size_t kHybridKemSharedSecretBytes = 64;
inline constexpr size_t kHybridKemPrivateKeyBytes = 2432;

struct HybridKemKeyPair {
  ByteVector public_key;
  ByteVector private_key;
};

/** X25519 + ML-KEM-768 (Kyber768 draft) — matches BoringSSL X25519Kyber768Draft00. */
class HybridKem {
public:
  static Roe<HybridKemKeyPair> GenerateKeyPair();
  static Roe<ByteVector> Encapsulate(const ByteVector& peer_public_key, std::string& key_init_b64_out);
  static Roe<ByteVector> Decapsulate(const ByteVector& private_key, const std::string& key_init_b64);
};

} // namespace pbr
