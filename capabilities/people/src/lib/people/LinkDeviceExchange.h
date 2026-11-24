#pragma once

#include "common/Secret.h"

#include "foundation/crypto/DataKeyVault.h"
#include "foundation/crypto/LinkHandshake.h"
#include "foundation/crypto/RelayId.h"
#include "lib/people/IdentityStore.h"

#include "common/Error.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The account's side of linking a device (multi-device M027; docs/contracts/LINK_DEVICE.md) and of a restore from the
 * recovery phrase (M023 §8), which bring an account onto an install the same way: from its recovery seed alone. UI,
 * the card fetch and push re-attach stay in feature.
 */
class LinkDeviceExchange {
public:
  /**
   * The main device's sealed reply to a new device's `offer`: the account's recovery seed, intent join, an expiry
   * `ttl_ms` from now, and `relay` when given (the account's preferred relay, when it is not the deployment's). Refused
   * for an account that is not registered or has no recovery seed (one minted before M023 s1: its keys derive from
   * nothing a link could carry).
   */
  static Roe<ByteVector> SealReply(const LocalIdentity& identity, std::span<const uint8_t> offer,
                                   std::optional<RelayAddress> relay, int64_t now_ms, int64_t ttl_ms = kLinkWindowMs);

  /**
   * Bring the account of `seed` onto this install: its signing and KEM keys and Account ID derived from the seed
   * (M023 §2) and the seed kept; this device's own keys (PeerId, device KEM) kept; registered; `card_intent` join (a
   * link: this device appends itself to the main device's list) or fresh (a restore). The DEK is this install's own:
   * on an empty vault a fresh one, wrapped with `pin` (`CreateWithDek`); an existing vault (unlocked, the identity
   * loaded under it) keeps its DEK and PIN. `nickname` replaces this install's when not empty.
   */
  static Roe<LocalIdentity> ImportAccount(IdentityStore& identity, DataKeyVault& vault, const ByteVector& seed,
                                          std::string_view card_intent, const std::string& nickname,
                                          const SecretText& pin);
};

} // namespace pbr
