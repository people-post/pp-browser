#pragma once

#include "domain/messaging/PaymentPromiseStore.h"
#include "domain/people/ContactsStore.h"
#include "common/Error.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Local avoid helper for payment promises (P002).
 * Sets contact TrustLevel::Blocked when a matching Account ID contact exists,
 * and stamps local_avoid on the promise receipt.
 */
class PaymentPromiseAvoid {
public:
  /**
   * Avoid the counterparty of `promise_id` relative to `local_account_id`.
   * Contact block is best-effort when a contact row exists; receipt stamp always applied.
   */
  static Roe<void> AvoidCounterparty(PaymentPromiseStore& promises, ContactsStore& contacts,
                                     const std::string& promise_id, const std::string& local_account_id);

  /**
   * True when contact trust is Blocked for other_account_id, or a local_avoid receipt exists
   * between local_account_id and other_account_id.
   */
  static bool ShouldAvoid(const PaymentPromiseStore& promises, const ContactsStore& contacts,
                          const std::string& local_account_id, const std::string& other_account_id);
};

} // namespace pbr
