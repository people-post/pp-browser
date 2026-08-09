#include "base/crypto/HybridKem.h"

#include "base/crypto/CryptoUtil.h"

#include <cstring>

#ifndef OPENSSL_UNSTABLE_EXPERIMENTAL_KYBER
#define OPENSSL_UNSTABLE_EXPERIMENTAL_KYBER
#endif

#include <openssl/bytestring.h>
#include <openssl/curve25519.h>
#include <openssl/crypto.h>
#include <openssl/experimental/kyber.h>

namespace pbr {

namespace {

bool AppendKyberPrivateKey(CBB* out, const KYBER_private_key& private_key) {
  return KYBER_marshal_private_key(out, &private_key) == 1;
}

Roe<ByteVector> FinishCbb(CBB* cbb) {
  uint8_t* data = nullptr;
  size_t len = 0;
  if (!CBB_finish(cbb, &data, &len)) {
    CBB_cleanup(cbb);
    return Error("Failed to finalize KEM blob");
  }
  ByteVector out(data, data + len);
  OPENSSL_free(data);
  return out;
}

} // namespace

Roe<HybridKemKeyPair> HybridKem::GenerateKeyPair() {
  HybridKemKeyPair keys;
  keys.public_key.resize(kHybridKemPublicKeyBytes);
  keys.private_key.resize(kHybridKemPrivateKeyBytes);

  uint8_t x25519_public[kHybridKemX25519PublicBytes];
  uint8_t x25519_private[kHybridKemX25519PublicBytes];
  X25519_keypair(x25519_public, x25519_private);

  uint8_t kyber_public_encoded[KYBER_PUBLIC_KEY_BYTES];
  KYBER_private_key kyber_private;
  KYBER_generate_key(kyber_public_encoded, &kyber_private);

  std::memcpy(keys.public_key.data(), x25519_public, sizeof(x25519_public));
  std::memcpy(keys.public_key.data() + sizeof(x25519_public), kyber_public_encoded, sizeof(kyber_public_encoded));

  std::memcpy(keys.private_key.data(), x25519_private, sizeof(x25519_private));
  CBB kyber_private_cbb;
  CBB_init(&kyber_private_cbb, KYBER_PRIVATE_KEY_BYTES);
  if (!AppendKyberPrivateKey(&kyber_private_cbb, kyber_private)) {
    CBB_cleanup(&kyber_private_cbb);
    return Error("Failed to marshal Kyber private key");
  }
  auto marshaled = FinishCbb(&kyber_private_cbb);
  if (!marshaled) {
    return marshaled.error();
  }
  if (marshaled->size() != KYBER_PRIVATE_KEY_BYTES) {
    return Error("Unexpected Kyber private key size");
  }
  std::memcpy(keys.private_key.data() + sizeof(x25519_private), marshaled->data(), marshaled->size());
  return keys;
}

Roe<ByteVector> HybridKem::Encapsulate(const ByteVector& peer_public_key, std::string& key_init_b64_out) {
  if (peer_public_key.size() != kHybridKemPublicKeyBytes) {
    return Error("Invalid hybrid KEM public key size");
  }

  uint8_t ephemeral_x25519_public[kHybridKemX25519PublicBytes];
  uint8_t ephemeral_x25519_private[kHybridKemX25519PublicBytes];
  X25519_keypair(ephemeral_x25519_public, ephemeral_x25519_private);

  ByteVector shared_secret(kHybridKemSharedSecretBytes, 0);
  if (X25519(shared_secret.data(), ephemeral_x25519_private, peer_public_key.data()) != 1) {
    return Error("X25519 encapsulation failed");
  }

  CBS peer_kyber_cbs;
  CBS_init(&peer_kyber_cbs, peer_public_key.data() + kHybridKemX25519PublicBytes, KYBER_PUBLIC_KEY_BYTES);
  KYBER_public_key peer_kyber_public;
  if (!KYBER_parse_public_key(&peer_kyber_public, &peer_kyber_cbs) || CBS_len(&peer_kyber_cbs) != 0) {
    return Error("Failed to parse peer Kyber public key");
  }

  uint8_t kyber_ciphertext[KYBER_CIPHERTEXT_BYTES];
  KYBER_encap(kyber_ciphertext, shared_secret.data() + kHybridKemX25519PublicBytes, &peer_kyber_public);

  CBB ciphertext_cbb;
  CBB_init(&ciphertext_cbb, kHybridKemCiphertextBytes);
  if (!CBB_add_bytes(&ciphertext_cbb, ephemeral_x25519_public, sizeof(ephemeral_x25519_public)) ||
      !CBB_add_bytes(&ciphertext_cbb, kyber_ciphertext, sizeof(kyber_ciphertext))) {
    CBB_cleanup(&ciphertext_cbb);
    return Error("Failed to encode hybrid KEM ciphertext");
  }
  auto ciphertext = FinishCbb(&ciphertext_cbb);
  if (!ciphertext) {
    return ciphertext.error();
  }
  if (ciphertext->size() != kHybridKemCiphertextBytes) {
    return Error("Unexpected hybrid KEM ciphertext size");
  }

  auto encoded = Base64Encode(*ciphertext);
  key_init_b64_out = std::move(encoded);
  return shared_secret;
}

Roe<ByteVector> HybridKem::Decapsulate(const ByteVector& private_key, const std::string& key_init_b64) {
  if (private_key.size() != kHybridKemPrivateKeyBytes) {
    return Error("Invalid hybrid KEM private key size");
  }

  auto ciphertext = Base64Decode(key_init_b64);
  if (!ciphertext) {
    return ciphertext.error();
  }
  if (ciphertext->size() != kHybridKemCiphertextBytes) {
    return Error("Invalid hybrid KEM ciphertext size");
  }

  ByteVector shared_secret(kHybridKemSharedSecretBytes, 0);
  if (X25519(shared_secret.data(), private_key.data(), ciphertext->data()) != 1) {
    return Error("X25519 decapsulation failed");
  }

  CBS kyber_private_cbs;
  CBS_init(&kyber_private_cbs, private_key.data() + kHybridKemX25519PublicBytes,
           private_key.size() - kHybridKemX25519PublicBytes);
  KYBER_private_key kyber_private;
  if (!KYBER_parse_private_key(&kyber_private, &kyber_private_cbs)) {
    return Error("Failed to parse local Kyber private key");
  }

  KYBER_decap(shared_secret.data() + kHybridKemX25519PublicBytes, ciphertext->data() + kHybridKemX25519PublicBytes,
              &kyber_private);
  return shared_secret;
}

} // namespace pbr
