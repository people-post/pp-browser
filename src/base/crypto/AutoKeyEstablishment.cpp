#include "base/crypto/AutoKeyEstablishment.h"

#include "base/crypto/CryptoConstants.h"
#include "base/crypto/CryptoUtil.h"

#include <sodium.h>

#include <cstring>

namespace pbr {

namespace {

Roe<ByteVector> HkdfMasterPsk(const ByteVector& ikm) {
  if (ikm.empty()) {
    return Error("Empty KEM shared secret");
  }
  EnsureSodiumInit();

  const std::string info = std::string(kAutoKeyHkdfInfoPrefix) + CryptoChannelToString(CryptoChannel::E2ePublic);
  unsigned char prk[crypto_kdf_hkdf_sha256_KEYBYTES];
  if (crypto_kdf_hkdf_sha256_extract(prk, reinterpret_cast<const unsigned char*>(kHkdfSalt), std::strlen(kHkdfSalt),
                                     ikm.data(), ikm.size()) != 0) {
    return Error("Auto-key HKDF extract failed");
  }
  ByteVector master_psk(kMasterPskSize);
  if (crypto_kdf_hkdf_sha256_expand(master_psk.data(), master_psk.size(), info.c_str(), info.size(), prk) != 0) {
    return Error("Auto-key HKDF expand failed");
  }
  return master_psk;
}

} // namespace

Roe<ByteVector> AutoKeyEstablishment::DeriveMasterPskFromSharedSecret(const ByteVector& kem_shared_secret) {
  if (kem_shared_secret.size() != kHybridKemSharedSecretBytes) {
    return Error("Invalid hybrid KEM shared secret size");
  }
  return HkdfMasterPsk(kem_shared_secret);
}

Roe<ByteVector> AutoKeyEstablishment::DeriveMasterPskFromKeyInit(const ByteVector& local_private_key,
                                                               const std::string& key_init_b64) {
  auto shared_secret = HybridKem::Decapsulate(local_private_key, key_init_b64);
  if (!shared_secret) {
    return shared_secret.error();
  }
  return DeriveMasterPskFromSharedSecret(*shared_secret);
}

Roe<AutoKeyEncapsulation> AutoKeyEstablishment::EncapsulateForRecipient(const ByteVector& peer_public_key) {
  AutoKeyEncapsulation result;
  auto shared_secret = HybridKem::Encapsulate(peer_public_key, result.key_init_b64);
  if (!shared_secret) {
    return shared_secret.error();
  }
  auto master_psk = DeriveMasterPskFromSharedSecret(*shared_secret);
  if (!master_psk) {
    return master_psk.error();
  }
  result.master_psk = std::move(*master_psk);
  return result;
}

Roe<std::string> AutoKeyEstablishment::HashKeyInitB64(const std::string& key_init_b64) {
  auto raw = Base64Decode(key_init_b64);
  if (!raw) {
    return raw.error();
  }
  if (raw->empty()) {
    return Error("Empty key_init");
  }
  EnsureSodiumInit();
  ByteVector digest(crypto_generichash_BYTES);
  if (crypto_generichash(digest.data(), digest.size(), raw->data(), raw->size(), nullptr, 0) != 0) {
    return Error("key_init hash failed");
  }
  return BytesToHex(digest);
}

} // namespace pbr
