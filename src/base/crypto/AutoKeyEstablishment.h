#pragma once

#include "base/crypto/CryptoTypes.h"
#include "base/crypto/HybridKem.h"

#include "common/Error.h"

#include <string>

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
  /** `peer_public_key` is the recipient account ML-KEM-768 public key. */
  static Roe<AutoKeyEncapsulation> EncapsulateForRecipient(const ByteVector& peer_public_key);
};

} // namespace pbr
