#include "foundation/crypto/HybridKem.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/TypedBlob.h"
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
  auto key_init = TypedKemCiphertext(encap->ciphertext);
  if (!key_init) {
    return key_init.error();
  }
  key_init_b64_out = Base64Encode(*key_init);
  return encap->shared_secret;
}

Roe<ByteVector> HybridKem::Decapsulate(const ByteVector& private_key, const std::string& key_init_b64) {
  const TypedRead ciphertext = ReadTypedBlobB64(::pp::BlobKind::kKemCiphertext, key_init_b64, "key_init");
  if (!ciphertext.ok()) {
    return ciphertext.ToError(); // an unknown KEM id is "update needed" (AppError::NewerFormat)
  }
  return ::pp::MlKem::Decapsulate(private_key, ciphertext.raw);
}

} // namespace pbr
