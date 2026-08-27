#include "base/crypto/MlDsa.h"

#include "base/crypto/CryptoUtil.h"

#include <sodium.h>

#include <string>

namespace pbr {

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
