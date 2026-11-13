#pragma once

#include "lib/pricing/IInitiationBillingBook.h"
#include "lib/pricing/PricingTypes.h"
#include "common/Error.h"
#include "common/Module.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Per-peer initiation billing state (P001).
 * Persists under profile `initiation_billing.json`. Keyed by the peer's Account ID (or a peer id where no account is
 * known). Schema 2; a 1 is stepped up on load, its `relay:` keys dropped (multi-relay MR001).
 */
class InitiationBillingStore : public Module, public IInitiationBillingBook {
public:
  static constexpr uint64_t kSchemaVersion = 2;

  explicit InitiationBillingStore(std::string data_dir);

  Roe<void> Load();
  Roe<void> Save() const;

  InitiationPeerBilling Get(const std::string& peer_identity) const override;
  Roe<void> Set(const std::string& peer_identity, InitiationPeerBilling row);

  /** Directory / lookup refresh — does not open the relationship. */
  Roe<void> SetFloor(const std::string& peer_identity, int64_t floor_minor);

  Roe<void> MarkOffered(const std::string& peer_identity, int64_t offer_minor, int64_t floor_minor) override;
  Roe<void> MarkOpen(const std::string& peer_identity) override;
  Roe<void> MarkClosed(const std::string& peer_identity);

  bool IsOpen(const std::string& peer_identity) const override;

private:
  std::string Path() const;
  Roe<void> SaveUnlocked() const;

  std::string data_dir_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, InitiationPeerBilling> rows_;
};

} // namespace pbr
