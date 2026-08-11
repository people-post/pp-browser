#include "base/crypto/MlDsa.h"

#include "base/crypto/CryptoUtil.h"

#include <mldsa_native.h>
#include <sodium.h>

#include <cstring>
#include <string>

namespace pbr {

Roe<MlDsaKeyPair> MlDsa::GenerateKeyPair() {
  EnsureSodiumInit();
  MlDsaKeyPair keys;
  keys.public_key.resize(kMlDsa65PublicKeyBytes);
  keys.secret_key.resize(kMlDsa65SecretKeyBytes);
  if (mldsa_keypair(keys.public_key.data(), keys.secret_key.data()) != 0) {
    return Error("ML-DSA-65 keygen failed");
  }
  return keys;
}

Roe<ByteVector> MlDsa::Sign(const ByteVector& secret_key, const ByteVector& message) {
  if (secret_key.size() != kMlDsa65SecretKeyBytes) {
    return Error("Invalid ML-DSA-65 secret key size");
  }
  EnsureSodiumInit();
  ByteVector sig(kMlDsa65SignatureBytes);
  if (mldsa_signature(sig.data(), message.data(), message.size(), nullptr, 0, secret_key.data()) != 0) {
    return Error("ML-DSA-65 sign failed");
  }
  return sig;
}

Roe<bool> MlDsa::Verify(const ByteVector& public_key, const ByteVector& message, const ByteVector& signature) {
  if (public_key.size() != kMlDsa65PublicKeyBytes) {
    return Error("Invalid ML-DSA-65 public key size");
  }
  if (signature.size() != kMlDsa65SignatureBytes) {
    return Error("Invalid ML-DSA-65 signature size");
  }
  EnsureSodiumInit();
  const int rc = mldsa_verify(signature.data(), message.data(), message.size(), nullptr, 0, public_key.data());
  if (rc == 0) {
    return true;
  }
  return false;
}

Roe<std::string> AccountIdFromMlDsaPublicKey(const ByteVector& public_key) {
  if (public_key.size() != kMlDsa65PublicKeyBytes) {
    return Error("Invalid ML-DSA-65 public key size");
  }
  EnsureSodiumInit();
  unsigned char hash[crypto_generichash_BYTES]; // 32 = BLAKE2b-256 default
  if (crypto_generichash(hash, sizeof(hash), public_key.data(), public_key.size(), nullptr, 0) != 0) {
    return Error("Account ID hash failed");
  }
  const size_t max_len = sodium_base64_ENCODED_LEN(sizeof(hash), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
  std::string b64(max_len, '\0');
  sodium_bin2base64(b64.data(), b64.size(), hash, sizeof(hash), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
  while (!b64.empty() && (b64.back() == '\0' || b64.back() == '\n')) {
    b64.pop_back();
  }
  return std::string("account:") + b64;
}

} // namespace pbr
