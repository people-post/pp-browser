#pragma once

#include "common/Error.h"
#include "common/thread/ThreadTypes.h"
#include "lib/delivery/IPeerSessionStore.h"
#include "lib/delivery/E2ePublicSessionLogic.h"
#include "lib/people/AccountCard.h"
#include "lib/people/PeerKemKeyStore.h"

#include <cstdint>
#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The e2e_public sessions a control-lane send goes under: with one of a peer's devices (the device pair's, M021 —
 * the chat stream's own), or with the peer's account (the account pair's: an account copy, a first contact). Opened on first use from the peer's KEM public key — the device's on its card, the
 * account's from the directory (M015); a session opened now carries the key_init the first send attaches. Any
 * thread.
 */
class E2ePublicSessionKeys {
public:
  struct Session {
    PeerSessionKey target_key;
    uint32_t epoch = 1;
    ByteVector master_psk;
    /**
     * The opener of a session we opened, while the peer has not been heard under it (this call may just have opened
     * it): every send under it attaches this.
     */
    std::optional<std::string> key_init_b64;
  };

  E2ePublicSessionKeys(IPeerSessionStore& psk_store, IPeerKemKeyResolver& kem_key_resolver);

  /** The account pair's session with `peer_identity`. */
  Roe<Session> Resolve(const std::string& peer_identity);
  /** The device pair's session with `device` of `peer_identity`'s (as its newest card lists it), opened to its KEM. */
  Roe<Session> ResolveDevice(const std::string& peer_identity, const AccountCardDevice& device);
  /** The peer's account KEM public key (directory, M015). */
  Roe<ByteVector> PeerKemPublicKey(const PeerSessionKey& key) const;

private:
  IPeerSessionStore& psk_store_;
  IPeerKemKeyResolver& kem_key_resolver_;
};

} // namespace pbr
