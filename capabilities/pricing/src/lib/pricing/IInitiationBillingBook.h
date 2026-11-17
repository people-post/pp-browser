#pragma once

#include "lib/pricing/PricingTypes.h"
#include "common/Error.h"

#include <cstdint>
#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

struct InitiationPeerBilling {
  InitiationBillingState state = InitiationBillingState::Closed;
  /**
   * The peer's floor: as Get gives it, the one PeerInitiationFloor picks; as kept, the one heard without a card (the
   * directory's entry, the peer's `charge_required`, an offer); 0 if unknown/missing.
   */
  int64_t floor_minor = 0;
  /** Outstanding or last offer from local initiator. */
  int64_t offer_minor = 0;
  /** Currency id (stub pp_credit). */
  std::string currency = kPricingCurrencyId;
  /** The floor a relay refused our send with (`payment_required`, MR005); none until one did. */
  std::optional<int64_t> relay_floor_minor;
  /** The issue time of the peer's card we kept when the relay said it (0: none); a newer card speaks over it. */
  int64_t relay_floor_card_issued_at = 0;
};

/** What the peer's newest kept account card says (multi-relay MR005): its floor, and when the card was issued. */
struct InitiationCardFloor {
  int64_t floor_minor = 0;
  int64_t issued_at = 0;
};

/**
 * The peer's floor, from the most authoritative source (multi-relay MR005): the relay's `payment_required` when it came
 * after the newest card we keep (the relay reads the card the peer registered, which may be newer than ours); else
 * that card's; else, with no card known, the floor heard otherwise (the directory's entry, `charge_required`).
 */
int64_t PeerInitiationFloor(const InitiationPeerBilling& row, const std::optional<InitiationCardFloor>& card);

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
