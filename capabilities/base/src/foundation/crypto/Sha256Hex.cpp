#include "foundation/crypto/Sha256Hex.h"

#include "foundation/crypto/CryptoUtil.h"

#include <sodium.h>

namespace pbr {

std::string Sha256Hex(const std::string_view data) {
  EnsureSodiumInit();
  ByteVector digest(crypto_hash_sha256_BYTES);
  crypto_hash_sha256(digest.data(), reinterpret_cast<const unsigned char*>(data.data()), data.size());
  return BytesToHex(digest);
}

} // namespace pbr
