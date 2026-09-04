#pragma once

#include "base/data/PaymentPromiseTypes.h"
#include "base/messaging/PaymentPromiseStore.h"
#include "base/people/ContactsStore.h"
#include "base/people/IdentityStore.h"
#include "common/Error.h"

#include <cstdint>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Local payment-promise lifecycle helpers (P002).
 * Create / accept / outcome / avoid using the unlocked account ML-DSA key.
 * No wire protocol or settlement — durable signed receipts only.
 */
class PaymentPromiseLifecycle {
public:
  struct OfferParams {
    /** Counterparty Account ID (`account:…`). */
    std::string counterparty_account_id;
    /** When true, local account is payer; otherwise local is payee. */
    bool local_is_payer = true;
    int64_t amount_minor = 0;
    std::string service_ref;
    std::string terms_hash_b64;
    std::string content_commitment_b64;
    PaymentPromiseReleaseRule release_rule = PaymentPromiseReleaseRule::PayerAck;
    /** 0 = no expiry. Absolute unix ms. */
    int64_t expires_at_ms = 0;
  };

  /** Create Offered receipt, sign with local account key, upsert. */
  static Roe<PaymentPromise> CreateOffer(PaymentPromiseStore& store, IdentityStore& identity,
                                         const OfferParams& params);

  /** Move Offered → Accepted and attach local counterparty signature. */
  static Roe<PaymentPromise> Accept(PaymentPromiseStore& store, IdentityStore& identity,
                                    const std::string& promise_id);

  /** Non-terminal progress marker (Accepted → Delivering). No new signature. */
  static Roe<PaymentPromise> MarkDelivering(PaymentPromiseStore& store, const std::string& promise_id);

  /**
   * Record a terminal outcome (Released / Rejected / Expired / Disputed),
   * sign outcome bytes with local account key, upsert.
   */
  static Roe<PaymentPromise> RecordOutcome(PaymentPromiseStore& store, IdentityStore& identity,
                                           const std::string& promise_id, PaymentPromiseState outcome,
                                           const std::string& note = {});

  /** Stamp local_avoid + best-effort contact Blocked for counterparty. */
  static Roe<void> AvoidCounterparty(PaymentPromiseStore& store, ContactsStore& contacts,
                                     IdentityStore& identity, const std::string& promise_id);
};

} // namespace pbr
