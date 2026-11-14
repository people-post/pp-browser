#pragma once

#include "domain/peers/AccountKeyResolver.h"
#include "lib/delivery/RelaySendCoordinator.h"
#include "common/Error.h"
#include "common/Module.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * A recipient's relays, for sending in list order (multi-relay R2): the list of the newest card we keep for its
 * account (KeptRelays, MR002), and — when every relay on it said the recipient is not registered there — its newest
 * card fetched: from its devices over the mesh (the card exchange, bounded by kDeviceAskTimeoutMs), then from the
 * directories of our relays and our other directory providers, which keep the card each account registered with. A card is kept only once it verifies
 * against the Account ID (AccountKeyResolver). Refresh blocks: a worker's (the send's).
 */
class RecipientRelays : public Module {
public:
  static constexpr int64_t kDeviceAskTimeoutMs = 8 * 1000;

  /** Asks `peer_id` for `account_id`'s card over the mesh (AccountCardService::Fetch); `done` once, any thread. */
  using DeviceAsk = std::function<void(const std::string& peer_id, const std::string& account_id,
                                       std::function<void(Roe<PeerCardRecord>)> done)>;
  /** The card a relay keeps for `account_id` (FetchStoredAccountCard); none when it keeps none. */
  using StoredCardFetch =
      std::function<Roe<std::optional<Object>>(const std::string& relay_origin, const std::string& account_id)>;

  RecipientRelays(AccountKeyResolver& keys, DeviceAsk ask_device);

  /**
   * Whose directories are asked for a stored card, in order: our relays, preferred first, then our other directory
   * providers (where we may have found the account).
   */
  void SetDirectories(std::vector<std::string> origins);
  /** Tests: how a stored card is fetched (default: over HTTP). */
  void SetStoredCardFetch(StoredCardFetch fetch) { fetch_stored_ = std::move(fetch); }

  /** The relays of `account_id`'s kept card, preferred first; empty: none known. */
  std::vector<std::string> Of(const std::string& account_id);
  /** Fetch `account_id`'s newest card and give its relays (blocking, bounded). */
  std::vector<std::string> Refresh(const std::string& account_id);
  /** For the relay sends (RelaySendCoordinator::SetRoutes); valid while this lives. */
  RelayRoutes Routes();

private:
  /** Asks the account's kept devices in turn until one gives a card that checks out; false when none did. */
  bool AskDevices(const std::string& account_id);
  void AskDirectories(const std::string& account_id);

  AccountKeyResolver& keys_;
  DeviceAsk ask_device_;
  StoredCardFetch fetch_stored_;
  std::mutex mutex_;
  std::vector<std::string> directories_;
};

} // namespace pbr
