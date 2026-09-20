#pragma once

#include "foundation/crypto/CryptoTypes.h"
#include "foundation/crypto/IPskSessionStore.h"

#include "common/Error.h"
#include "common/PbrCompat.h"

#include <optional>
#include <string>

namespace pbr {

struct E2ePublicPskEnsureResult {
  ByteVector master_psk;
  /** Set only when AutoKey encapsulation just created the session (attach on first send). */
  std::optional<std::string> key_init_b64;
  bool created = false;
};

/**
 * Load existing e2e_public master PSK, or AutoKey-encapsulate to `peer_kem_public` and save.
 * Callers that wrap before the first outbound message must attach `key_init_b64` on that send
 * so the peer can open the session.
 */
Roe<E2ePublicPskEnsureResult> EnsureE2ePublicMasterPsk(IPskSessionStore& psk_store,
                                                      const ChatTargetKey& target_key,
                                                      uint32_t session_epoch,
                                                      const ByteVector& peer_kem_public);

/** Derive the pairwise session key used for CallMediaKey wrap (channel e2e_public). */
Roe<ByteVector> DeriveE2ePublicSessionKey(const ByteVector& master_psk, uint32_t session_epoch);

} // namespace pbr
