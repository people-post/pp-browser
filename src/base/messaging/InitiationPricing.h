#pragma once

#include "base/data/PricingTypes.h"
#include "common/Error.h"

#include <cstdint>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Pure helpers for initiation / media-relay payment gates (P001).
 * Settlement is unavailable — positive amounts are unpayable.
 */
struct InitiationPricing {
  /** Reject when a charging peer sees offer below floor. */
  static Roe<void> CheckOfferAgainstFloor(int64_t offer_minor, int64_t floor_minor);

  /**
   * Outbound initiate/dial: block when amount > 0 and rails unavailable.
   * Returns Error with stable prefix `payment_unavailable:` for UI.
   */
  static Roe<void> CheckOutboundPayable(int64_t amount_minor);

  /**
   * Media-relay quote: rate == 0 proceeds; rate > 0 needs payment (unavailable).
   * Returns Error `payment_unavailable:media_relay` when blocked.
   */
  static Roe<void> CheckRelayQuotePayable(double rate);

  /** Default offer for a floor: meet floor exactly (tips later). */
  static int64_t DefaultOfferForFloor(int64_t floor_minor);
};

} // namespace pbr
