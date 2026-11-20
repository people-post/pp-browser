#pragma once

#include "lib/pricing/IInitiationBillingBook.h"
#include "lib/pricing/PricingTypes.h"
#include "common/Error.h"
#include "common/Module.h"

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Per-peer initiation billing state (P001).
 * Persists under profile `initiation_billing.json`. Keyed by the peer's Account ID (or a peer id where no account is
 * known). Schema 2, also its floor: an older, unversioned or newer file is refused (release 1 starts clean,
 * release-compatibility RC017).
 *
 * A peer's floor is read from its newest kept account card when one is known (CardFloors, multi-relay MR005), the
 * relay's `payment_required` over a card it came after, and the directory's / `charge_required`'s only without a card
 * (PeerInitiationFloor): Get gives that one.
 */
class InitiationBillingStore : public Module, public IInitiationBillingBook {
public:
  static constexpr uint64_t kSchemaVersion = 2;
  static constexpr uint64_t kFloorSchemaVersion = 2;

  explicit InitiationBillingStore(std::string data_dir);

  Roe<void> Load();
  Roe<void> Save() const;

  /** What the peer's newest kept card says; none: no card known (or no account). */
  using CardFloors = std::function<std::optional<InitiationCardFloor>(const std::string& peer_identity)>;
  /** Where the peers' cards are read (the owner's kept cards); none: no card is ever known. */
  void SetCardFloors(CardFloors card_floors);

  InitiationPeerBilling Get(const std::string& peer_identity) const override;
  Roe<void> Set(const std::string& peer_identity, InitiationPeerBilling row);

  /** Directory / lookup refresh, `charge_required` — does not open the relationship. Used while no card is known. */
  Roe<void> SetFloor(const std::string& peer_identity, int64_t floor_minor);
  /**
   * The relay refused our send to the peer with `floor_minor` (`payment_required`): the relay's word, until a newer card
   * of the peer is kept. True when the kept card says another price or none is known: the card is worth refreshing.
   */
  Roe<bool> SetRelayFloor(const std::string& peer_identity, int64_t floor_minor);

  Roe<void> MarkOffered(const std::string& peer_identity, int64_t offer_minor, int64_t floor_minor) override;
  Roe<void> MarkOpen(const std::string& peer_identity) override;
  Roe<void> MarkClosed(const std::string& peer_identity);

  bool IsOpen(const std::string& peer_identity) const override;

private:
  std::string Path() const;
  Roe<void> SaveUnlocked() const;

  std::optional<InitiationCardFloor> CardFloorOf(const std::string& peer_identity) const;

  std::string data_dir_;
  CardFloors card_floors_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, InitiationPeerBilling> rows_;
};

} // namespace pbr
