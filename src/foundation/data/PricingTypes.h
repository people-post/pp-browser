#pragma once

#include <cstdint>
#include <string>

namespace pbr {

/** Internal currency id until product name/symbol is chosen (P001). */
inline constexpr const char* kPricingCurrencyId = "pp_credit";
/** User-facing label for confirm copy (not localized key). */
inline constexpr const char* kPricingCurrencyDisplayName = "Credits";

/** True when amount is free — protocol branches on this, not on mode labels. */
inline constexpr bool IsFreeAmount(const int64_t amount_minor) {
  return amount_minor <= 0;
}

/** Media-relay quote rate: free when <= 0 (P001 — no volunteer special-case). */
inline constexpr bool IsFreeRelayRate(const double rate) {
  return rate <= 0.0;
}

/** UX label only — do not use for protocol branching. */
inline constexpr const char* PricingModeLabel(const int64_t amount_minor) {
  return IsFreeAmount(amount_minor) ? "volunteer" : "paid";
}

inline constexpr const char* PricingModeLabelForRate(const double rate) {
  return IsFreeRelayRate(rate) ? "volunteer" : "paid";
}

/** Offer meets advertised floor (floor missing/0 always accepts any non-negative offer). */
inline constexpr bool OfferMeetsFloor(const int64_t offer_minor, const int64_t floor_minor) {
  if (offer_minor < 0) {
    return false;
  }
  return offer_minor >= floor_minor;
}

/** Settlement rails are not wired — any positive amount is currently unpayable. */
inline constexpr bool PaymentRailsAvailable() {
  return false;
}

inline constexpr bool CanPayAmount(const int64_t amount_minor) {
  return IsFreeAmount(amount_minor) || PaymentRailsAvailable();
}

enum class InitiationBillingState : uint8_t {
  Closed = 0,
  Offered = 1,
  Open = 2,
};

inline constexpr const char* InitiationBillingStateToString(const InitiationBillingState state) {
  switch (state) {
  case InitiationBillingState::Offered:
    return "offered";
  case InitiationBillingState::Open:
    return "open";
  case InitiationBillingState::Closed:
  default:
    return "closed";
  }
}

inline InitiationBillingState InitiationBillingStateFromString(const std::string& value) {
  if (value == "offered") {
    return InitiationBillingState::Offered;
  }
  if (value == "open") {
    return InitiationBillingState::Open;
  }
  return InitiationBillingState::Closed;
}

enum class InitiationChargeDecision : uint8_t {
  Waive = 0,
  TakeAll = 1,
};

inline constexpr const char* InitiationChargeDecisionToWire(const InitiationChargeDecision d) {
  return d == InitiationChargeDecision::TakeAll ? "take_all" : "waive";
}

inline InitiationChargeDecision InitiationChargeDecisionFromWire(const std::string& value) {
  if (value == "take_all") {
    return InitiationChargeDecision::TakeAll;
  }
  return InitiationChargeDecision::Waive;
}

} // namespace pbr
