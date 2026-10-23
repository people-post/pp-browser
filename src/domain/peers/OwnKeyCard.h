#pragma once

#include "common/directory/KeyCard.h"
#include "lib/people/IdentityStore.h"

#include "common/Error.h"

#include <mutex>
#include <optional>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * This account's own key card (docs/contracts/ACCOUNT_CARD.md#key-card), carried with every key_init:
 * built once — the KEM binding is an account signature — and again only when the identity's keys change
 * (a linked or restored profile). Thread-safe.
 */
class OwnKeyCard {
public:
  explicit OwnKeyCard(IdentityStore& identity) : identity_(identity) {}

  Roe<KeyCard> Get();

private:
  IdentityStore& identity_;
  std::mutex mutex_;
  std::optional<KeyCard> card_;
};

} // namespace pbr
