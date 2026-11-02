#pragma once

#include "foundation/crypto/CryptoTypes.h"

#include "common/Error.h"

#include "crypto/MlKem.h"

#include <cstddef>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** ML-KEM-768 sizes — aliases kept for call-site stability (`HybridKem` name). */
inline constexpr size_t kHybridKemPublicKeyBytes = ::pp::kMlKem768PublicKeyBytes;
inline constexpr size_t kHybridKemCiphertextBytes = ::pp::kMlKem768CiphertextBytes;
inline constexpr size_t kHybridKemSharedSecretBytes = ::pp::kMlKem768SharedSecretBytes;
inline constexpr size_t kHybridKemPrivateKeyBytes = ::pp::kMlKem768PrivateKeyBytes;

struct HybridKemKeyPair {
  ByteVector public_key;
  ByteVector private_key;
};

/**
 * ML-KEM-768 encaps/decaps for `e2e_public` key_init.
 * Class name kept for call-site stability; wraps pp::MlKem + base64 wire encoding.
 */
class HybridKem {
public:
  static Roe<HybridKemKeyPair> GenerateKeyPair();
  /** Deterministic ML-KEM-768 from 64-byte coins. */
  static Roe<HybridKemKeyPair> GenerateKeyPairFromSeed(const ByteVector& coins);
  static Roe<ByteVector> Encapsulate(const ByteVector& peer_public_key, std::string& key_init_b64_out);
  static Roe<ByteVector> Decapsulate(const ByteVector& private_key, const std::string& key_init_b64);
};

} // namespace pbr
