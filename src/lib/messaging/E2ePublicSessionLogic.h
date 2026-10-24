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
  /**
   * The key_init to attach to this send: set while the session is one we opened and the peer has
   * not been heard under it yet (a lost opening send must not lock the pair out).
   */
  std::optional<std::string> key_init_b64;
  bool created = false;
};

/** A session's id: the hash of the key_init that opened it (hex). Both sides compute the same. */
std::string AutoKeySessionId(const std::string& key_init_b64);

/** Which session an e2e_public envelope was read under. */
struct AutoKeyRead {
  ByteVector master_psk;
  /** Read under the session the envelope's key_init opens (not the one we had stored). */
  bool via_key_init = false;
};

/** Session key for CallMediaKey wrap, plus optional first-send AutoKey `key_init`. */
struct EnsuredE2ePublicSessionKey {
  ByteVector session_key;
  std::optional<std::string> first_message_key_init_b64;
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

/**
 * What reading an authenticated envelope (signature verified) proves about the pair's session, and
 * the session we keep:
 * - read under our stored session: the peer has it — a session we opened is confirmed;
 * - read under the envelope's key_init (ours could not read it, or we had none): the peer is on that
 *   session, and we move to it — unless both opened at once and ours, unconfirmed, has the lower id
 *   (the peer moves to ours instead; both sides decide the same) — or unless the envelope is older
 *   (its signed timestamp, the peer's clock) than one the peer already sent under our session: a late
 *   or replayed opener does not roll the pair back;
 * - read under a key_init for a later epoch than ours: the peer restarted the pair there (D014) — we move
 *   to that epoch and session, the previous one kept (retired) for late messages.
 */
Roe<void> ReconcileE2ePublicSession(IPskSessionStore& psk_store, const ChatTargetKey& target_key,
                                    uint32_t session_epoch, const AutoKeyRead& read,
                                    const std::optional<std::string>& envelope_key_init_b64,
                                    int64_t envelope_timestamp_ms);

/** Derive the pairwise session key used for CallMediaKey wrap (channel e2e_public). */
/**
 * The pair moved from `old_epoch` to `new_epoch` because this device lost its numbering (D014, #378): the
 * session is retired at `old_epoch` (late messages still read), none is current — the next send opens one
 * at `new_epoch` with a key_init the peer follows — and the restart mark is cleared.
 */
Roe<void> RestartE2ePublicSession(IPskSessionStore& psk_store, const ChatTargetKey& target_key, uint32_t old_epoch,
                                  uint32_t new_epoch, int64_t now_ms);

Roe<ByteVector> DeriveE2ePublicSessionKey(const ByteVector& master_psk, uint32_t session_epoch);

} // namespace pbr
