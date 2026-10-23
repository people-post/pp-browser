#pragma once

#include "common/Error.h"
#include "common/thread/ThreadTypes.h"
#include "foundation/crypto/IPskSessionStore.h"
#include "lib/messaging/E2ePublicSessionLogic.h"
#include "lib/messaging/PeerKemKeyStore.h"

#include <cstdint>
#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The e2e_public session between this account and a peer's (AutoKey; one per account pair, not per
 * thread): the control lane seals under it, and a call's media-key wrap rides it. Opened on first use
 * from the peer's KEM public key (the directory, M015); a session opened now carries the key_init the
 * first send attaches. Any thread.
 */
class E2ePublicSessionKeys {
public:
  struct Session {
    ChatTargetKey target_key;
    uint32_t epoch = 1;
    ByteVector master_psk;
    /** Set when this call just opened the session (attach on the first send). */
    std::optional<std::string> key_init_b64;
  };

  E2ePublicSessionKeys(IPskSessionStore& psk_store, IPeerKemKeyResolver& kem_key_resolver);

  Roe<Session> Resolve(const std::string& peer_identity);
  /** The session's current key (a call invite's media-key wrap), opening the session if needed. */
  Roe<EnsuredE2ePublicSessionKey> Ensure(const std::string& peer_identity);
  /** The peer's account KEM public key (directory, M015). */
  Roe<ByteVector> PeerKemPublicKey(const ChatTargetKey& key) const;

private:
  IPskSessionStore& psk_store_;
  IPeerKemKeyResolver& kem_key_resolver_;
};

} // namespace pbr
