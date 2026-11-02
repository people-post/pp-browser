#include "foundation/crypto/HybridKem.h"

#include "foundation/crypto/CryptoUtil.h"
#include "common/PbrCompat.h"

namespace pbr {

Roe<HybridKemKeyPair> HybridKem::GenerateKeyPair() {
  auto keys = ::pp::MlKem::GenerateKeyPair();
  if (!keys) {
    return keys.error();
  }
  HybridKemKeyPair out;
  out.public_key = std::move(keys->public_key);
  out.private_key = std::move(keys->private_key);
  return out;
}

Roe<HybridKemKeyPair> HybridKem::GenerateKeyPairFromSeed(const ByteVector& coins) {
  auto keys = ::pp::MlKem::GenerateKeyPairFromSeed(coins);
  if (!keys) {
    return keys.error();
  }
  HybridKemKeyPair out;
  out.public_key = std::move(keys->public_key);
  out.private_key = std::move(keys->private_key);
  return out;
}

Roe<ByteVector> HybridKem::Encapsulate(const ByteVector& peer_public_key, std::string& key_init_b64_out) {
  auto encap = ::pp::MlKem::Encapsulate(peer_public_key);
  if (!encap) {
    return encap.error();
  }
  key_init_b64_out = Base64Encode(encap->ciphertext);
  return encap->shared_secret;
}

Roe<ByteVector> HybridKem::Decapsulate(const ByteVector& private_key, const std::string& key_init_b64) {
  auto ciphertext = Base64Decode(key_init_b64);
  if (!ciphertext) {
    return ciphertext.error();
  }
  return ::pp::MlKem::Decapsulate(private_key, *ciphertext);
}

} // namespace pbr
