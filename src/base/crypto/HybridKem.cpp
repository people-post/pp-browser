#include "base/crypto/HybridKem.h"

#include "base/crypto/CryptoUtil.h"

#include <mlkem_native.h>

#include <cstring>

namespace pbr {

Roe<HybridKemKeyPair> HybridKem::GenerateKeyPair() {
  EnsureSodiumInit();
  HybridKemKeyPair keys;
  keys.public_key.resize(kHybridKemPublicKeyBytes);
  keys.private_key.resize(kHybridKemPrivateKeyBytes);
  if (mlkem_keypair(keys.public_key.data(), keys.private_key.data()) != 0) {
    return Error("ML-KEM-768 keygen failed");
  }
  return keys;
}

Roe<ByteVector> HybridKem::Encapsulate(const ByteVector& peer_public_key, std::string& key_init_b64_out) {
  if (peer_public_key.size() != kHybridKemPublicKeyBytes) {
    return Error("Invalid ML-KEM-768 public key size");
  }
  EnsureSodiumInit();

  ByteVector ciphertext(kHybridKemCiphertextBytes);
  ByteVector shared_secret(kHybridKemSharedSecretBytes);
  if (mlkem_enc(ciphertext.data(), shared_secret.data(), peer_public_key.data()) != 0) {
    return Error("ML-KEM-768 encapsulation failed");
  }
  key_init_b64_out = Base64Encode(ciphertext);
  return shared_secret;
}

Roe<ByteVector> HybridKem::Decapsulate(const ByteVector& private_key, const std::string& key_init_b64) {
  if (private_key.size() != kHybridKemPrivateKeyBytes) {
    return Error("Invalid ML-KEM-768 private key size");
  }
  auto ciphertext = Base64Decode(key_init_b64);
  if (!ciphertext) {
    return ciphertext.error();
  }
  if (ciphertext->size() != kHybridKemCiphertextBytes) {
    return Error("Invalid ML-KEM-768 ciphertext size");
  }
  EnsureSodiumInit();

  ByteVector shared_secret(kHybridKemSharedSecretBytes);
  if (mlkem_dec(shared_secret.data(), ciphertext->data(), private_key.data()) != 0) {
    return Error("ML-KEM-768 decapsulation failed");
  }
  return shared_secret;
}

} // namespace pbr
