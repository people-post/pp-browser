#pragma once

#include "common/directory/KeyCard.h"
#include "domain/peers/AccountKeyResolver.h"
#include "lib/messaging/AccountCard.h"
#include "lib/people/IdentityStore.h"

#include "common/Error.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * This account's own cards (docs/contracts/ACCOUNT_CARD.md): the key card, and the account card — the
 * account's device list (M021), kept with the other accounts' cards. A device issues a new card only when
 * what it says changes (this device's keys, the nickname, the devices), from the newest one it knows, so
 * the account's devices converge on one list. Thread-safe.
 */
class OwnCards {
public:
  OwnCards(IdentityStore& identity, AccountKeyResolver& cards) : identity_(identity), cards_(cards) {}

  Roe<KeyCard> Keys();
  /** The account card: the newest this device knows, re-issued (this device first) if it does not list this device as it is. */
  Roe<AccountCard> Card(int64_t now_ms);
  /**
   * A card of our own account from a sibling (the link bundle, a sibling's notice): checked and kept. Until
   * only a master issues cards (M021 d4) siblings may issue concurrently, so a device it does not list but
   * ours does is kept: the card is re-issued with both. The nickname is the account's: the newest card's
   * becomes this device's too.
   */
  Roe<AccountCard> Learn(const AccountCard& card, int64_t now_ms);
  /** This device's entry on the card. */
  Roe<AccountCardDevice> Self();
  /** This device's PeerId (its mesh identity, from the device key). */
  Roe<std::string> DevicePeerId();

private:
  Roe<AccountCard> IssueLocked(const std::optional<AccountCard>& base, const std::vector<AccountCardDevice>& also,
                               const std::string& nickname, int64_t now_ms);
  Roe<std::optional<AccountCard>> NewestLocked();

  IdentityStore& identity_;
  AccountKeyResolver& cards_;
  std::mutex mutex_;  // issuing a card
  std::mutex keys_mutex_;
  std::optional<KeyCard> keys_;
};

} // namespace pbr
