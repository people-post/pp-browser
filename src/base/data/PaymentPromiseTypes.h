#pragma once

#include "base/data/PricingTypes.h"

#include <cstdint>
#include <string>

namespace pbr {

/**
 * Local payment promise + outcome receipts (P002).
 * App-layer lifecycle for “promise before service / release after delivery.”
 * Settlement rails and public reputation are out of scope — signed artifacts only.
 */

enum class PaymentPromiseState : uint8_t {
  Offered = 0,
  Accepted = 1,
  Delivering = 2,
  Released = 3,
  Rejected = 4,
  Expired = 5,
  Disputed = 6,
};

enum class PaymentPromiseReleaseRule : uint8_t {
  /** Payer must ack release (default for subjective services). */
  PayerAck = 0,
  /** Both parties must ack release. */
  DualAck = 1,
  /** Auto-release after expires_at_ms when evidence is present (meters later). */
  TimeoutAutoRelease = 2,
};

inline constexpr const char* PaymentPromiseStateToString(const PaymentPromiseState state) {
  switch (state) {
  case PaymentPromiseState::Offered:
    return "offered";
  case PaymentPromiseState::Accepted:
    return "accepted";
  case PaymentPromiseState::Delivering:
    return "delivering";
  case PaymentPromiseState::Released:
    return "released";
  case PaymentPromiseState::Rejected:
    return "rejected";
  case PaymentPromiseState::Expired:
    return "expired";
  case PaymentPromiseState::Disputed:
    return "disputed";
  }
  return "offered";
}

inline PaymentPromiseState PaymentPromiseStateFromString(const std::string& value) {
  if (value == "accepted") {
    return PaymentPromiseState::Accepted;
  }
  if (value == "delivering") {
    return PaymentPromiseState::Delivering;
  }
  if (value == "released") {
    return PaymentPromiseState::Released;
  }
  if (value == "rejected") {
    return PaymentPromiseState::Rejected;
  }
  if (value == "expired") {
    return PaymentPromiseState::Expired;
  }
  if (value == "disputed") {
    return PaymentPromiseState::Disputed;
  }
  return PaymentPromiseState::Offered;
}

inline constexpr const char* PaymentPromiseReleaseRuleToString(const PaymentPromiseReleaseRule rule) {
  switch (rule) {
  case PaymentPromiseReleaseRule::DualAck:
    return "dual_ack";
  case PaymentPromiseReleaseRule::TimeoutAutoRelease:
    return "timeout_auto_release";
  case PaymentPromiseReleaseRule::PayerAck:
  default:
    return "payer_ack";
  }
}

inline PaymentPromiseReleaseRule PaymentPromiseReleaseRuleFromString(const std::string& value) {
  if (value == "dual_ack") {
    return PaymentPromiseReleaseRule::DualAck;
  }
  if (value == "timeout_auto_release") {
    return PaymentPromiseReleaseRule::TimeoutAutoRelease;
  }
  return PaymentPromiseReleaseRule::PayerAck;
}

inline constexpr bool PaymentPromiseStateIsTerminal(const PaymentPromiseState state) {
  switch (state) {
  case PaymentPromiseState::Released:
  case PaymentPromiseState::Rejected:
  case PaymentPromiseState::Expired:
  case PaymentPromiseState::Disputed:
    return true;
  case PaymentPromiseState::Offered:
  case PaymentPromiseState::Accepted:
  case PaymentPromiseState::Delivering:
    return false;
  }
  return false;
}

/** Durable local receipt for one promised payment obligation. */
struct PaymentPromise {
  std::string promise_id;
  /** Account IDs (`account:…`). */
  std::string payer_account_id;
  std::string payee_account_id;
  int64_t amount_minor = 0;
  std::string currency = kPricingCurrencyId;
  /** Opaque service reference (thread id, job id, hop quote id, …). */
  std::string service_ref;
  /** Optional BLAKE2b-256 (base64) of human-readable terms. */
  std::string terms_hash_b64;
  /** Optional content commitment (base64) — hash of delivery payload, not the payload. */
  std::string content_commitment_b64;
  PaymentPromiseReleaseRule release_rule = PaymentPromiseReleaseRule::PayerAck;
  int64_t created_at_ms = 0;
  /** 0 = no expiry. */
  int64_t expires_at_ms = 0;
  PaymentPromiseState state = PaymentPromiseState::Offered;

  /** ML-DSA-65 signatures (base64) over canonical promise bytes. */
  std::string payer_signature_b64;
  std::string payee_signature_b64;

  /** Outcome attestation (terminal states). */
  std::string outcome_actor_account_id;
  std::string outcome_signature_b64;
  int64_t outcome_at_ms = 0;
  std::string outcome_note;

  /**
   * Local-only: counterparty was marked avoid/blocked because of this promise.
   * Not part of signed bytes; never treat as remote evidence.
   */
  bool local_avoid = false;
};

} // namespace pbr
