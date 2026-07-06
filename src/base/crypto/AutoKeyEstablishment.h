#pragma once

#include "base/crypto/CryptoTypes.h"
#include "base/crypto/HybridKem.h"
#include "base/crypto/IPskSessionStore.h"
#include "base/messaging/ThreadTypes.h"

#include "common/Error.h"

#include <optional>
#include <string>

namespace pbr {

struct AutoKeyEncapsulation {
  ByteVector master_psk;
  std::string key_init_b64;
};

/** E024 — hybrid KEM → master_psk for e2e_public. */
class AutoKeyEstablishment {
public:
  static Roe<ByteVector> DeriveMasterPskFromSharedSecret(const ByteVector& kem_shared_secret);
  static Roe<ByteVector> DeriveMasterPskFromKeyInit(const ByteVector& local_private_key,
                                                    const std::string& key_init_b64);
  static Roe<AutoKeyEncapsulation> EncapsulateForRecipient(const ByteVector& peer_public_key);
  static Roe<ByteVector> ResolveOrDeriveMasterPsk(const RelayEnvelope& envelope, const ChatTargetKey& target_key,
                                                  IPskSessionStore& psk_store,
                                                  const ByteVector& local_private_key);
};

} // namespace pbr
