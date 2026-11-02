#pragma once

#include "lib/pricing/PricingTypes.h"
#include "common/Error.h"

#include <cstdint>
#include <string>
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
 * Per-peer initiation billing (P001) as a feature that places or answers paid invites reads and moves
 * it: a peer's row, whether the pair is open, an offer made, the pair opened.
 */
class IInitiationBillingBook {
public:
  virtual ~IInitiationBillingBook() = default;

  virtual InitiationPeerBilling Get(const std::string& peer_identity) const = 0;
  virtual bool IsOpen(const std::string& peer_identity) const = 0;
  virtual Roe<void> MarkOffered(const std::string& peer_identity, int64_t offer_minor, int64_t floor_minor) = 0;
  virtual Roe<void> MarkOpen(const std::string& peer_identity) = 0;
};

} // namespace pbr
