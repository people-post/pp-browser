#pragma once

#include "base/data/PricingTypes.h"
#include "common/Error.h"
#include "common/Module.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include "common/PbrCompat.h"

namespace pbr {

struct InitiationPeerBilling {
  InitiationBillingState state = InitiationBillingState::Closed;
  /** Last known peer floor (directory); 0 if unknown/missing. */
  int64_t floor_minor = 0;
  /** Outstanding or last offer from local initiator. */
  int64_t offer_minor = 0;
  /** Currency id (stub pp_credit). */
  std::string currency = kPricingCurrencyId;
};

/**
 * Per-peer initiation billing state (P001).
 * Persists under profile `initiation_billing.json`. Keyed by peer identity string
 * (relay:… or peer id as used elsewhere).
 */
class InitiationBillingStore : public Module {
public:
  explicit InitiationBillingStore(std::string data_dir);

  Roe<void> Load();
  Roe<void> Save() const;

  InitiationPeerBilling Get(const std::string& peer_identity) const;
  Roe<void> Set(const std::string& peer_identity, InitiationPeerBilling row);

  /** Directory / lookup refresh — does not open the relationship. */
  Roe<void> SetFloor(const std::string& peer_identity, int64_t floor_minor);

  Roe<void> MarkOffered(const std::string& peer_identity, int64_t offer_minor, int64_t floor_minor);
  Roe<void> MarkOpen(const std::string& peer_identity);
  Roe<void> MarkClosed(const std::string& peer_identity);

  bool IsOpen(const std::string& peer_identity) const;

private:
  std::string Path() const;
  Roe<void> SaveUnlocked() const;

  std::string data_dir_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, InitiationPeerBilling> rows_;
};

} // namespace pbr
