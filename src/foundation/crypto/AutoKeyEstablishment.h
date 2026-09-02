#pragma once

#include "foundation/crypto/CryptoTypes.h"
#include "foundation/crypto/HybridKem.h"

#include "common/Error.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

struct AutoKeyEncapsulation {
  ByteVector master_psk;
  std::string key_init_b64;
};

/** E024 — account ML-KEM-768 → master_psk for e2e_public (M015). */
class AutoKeyEstablishment {
public:
  static Roe<ByteVector> DeriveMasterPskFromSharedSecret(const ByteVector& kem_shared_secret);
  static Roe<ByteVector> DeriveMasterPskFromKeyInit(const ByteVector& local_private_key,
                                                    const std::string& key_init_b64);
  /** `peer_public_key` is the recipient account or conversation ML-KEM-768 public key. */
  static Roe<AutoKeyEncapsulation> EncapsulateForRecipient(const ByteVector& peer_public_key);
  /** BLAKE2b-256 of decoded `key_init` bytes, lowercase hex (E027 `key_init_hash`). */
  static Roe<std::string> HashKeyInitB64(const std::string& key_init_b64);
};

} // namespace pbr
