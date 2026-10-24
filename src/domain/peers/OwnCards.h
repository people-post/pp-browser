#pragma once

#include "common/directory/KeyCard.h"
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
 * This account's own cards (docs/contracts/ACCOUNT_CARD.md), as this device issues them: the key card
 * carried with every key_init, and the account card served to whoever asks for it. Each is signed once
 * and again only when what it says changes (keys, nickname, devices). Thread-safe.
 */
class OwnCards {
public:
  explicit OwnCards(IdentityStore& identity) : identity_(identity) {}

  Roe<KeyCard> Keys();
  /**
   * The account card: this device first, then `siblings` — the account's other devices this one knows
   * of (at most kMaxAccountCardDevices in all).
   */
  Roe<AccountCard> Card(const std::vector<std::string>& siblings, int64_t now_ms);
  /** This device's PeerId (its mesh identity, from the device key). */
  Roe<std::string> DevicePeerId();

private:
  IdentityStore& identity_;
  std::mutex mutex_;
  std::optional<KeyCard> keys_;
  std::optional<AccountCard> card_;
};

} // namespace pbr
