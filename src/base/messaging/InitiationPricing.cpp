#include "base/messaging/InitiationPricing.h"

namespace pbr {

Roe<void> InitiationPricing::CheckOfferAgainstFloor(const int64_t offer_minor, const int64_t floor_minor) {
  if (!OfferMeetsFloor(offer_minor, floor_minor)) {
    return Error("offer_too_low: offer below initiation floor");
  }
  return {};
}

Roe<void> InitiationPricing::CheckOutboundPayable(const int64_t amount_minor) {
  if (CanPayAmount(amount_minor)) {
    return {};
  }
  return Error(std::string("payment_unavailable: cannot pay ") + std::to_string(amount_minor) + " " +
               kPricingCurrencyDisplayName + " yet");
}

Roe<void> InitiationPricing::CheckRelayQuotePayable(const double rate) {
  if (IsFreeRelayRate(rate)) {
    return {};
  }
  if (PaymentRailsAvailable()) {
    return {};
  }
  return Error("payment_unavailable:media_relay");
}

int64_t InitiationPricing::DefaultOfferForFloor(const int64_t floor_minor) {
  return floor_minor < 0 ? 0 : floor_minor;
}

} // namespace pbr
