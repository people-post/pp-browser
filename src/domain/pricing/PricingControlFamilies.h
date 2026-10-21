#pragma once

#include "domain/pricing/PaymentPromiseInbound.h"

#include "common/chat/SystemControl.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

class InitiationBillingStore;
class PaymentPromiseStore;

inline constexpr const char* kInitiationBillingFamily = "billing";
inline constexpr const char* kPaymentPromiseFamily = "payment promise";

/**
 * Initiation billing (charge_required, initiation_offer, initiation_accept) as a system-control
 * family: applied in order, shown as its own text. Billing state is keyed on the authenticated
 * sender; a message naming anyone else is refused, so no peer can close or re-price another's
 * relationship. `local_floor` is our own initiation floor (0: none).
 */
SystemControlFamily MakeInitiationBillingFamily(InitiationBillingStore& store, std::function<int64_t()> local_floor);

/**
 * Payment promises (offer, accept, outcome) as a system-control family: applied in order, shown as
 * their own text. One is staged only when its sender is our counterparty on it and it passes
 * MergeInboundPaymentPromise (signed, a legal change of our copy); committing it takes the user's
 * accept (P003), which checks again. `local_account` is our account id.
 */
SystemControlFamily MakePaymentPromiseFamily(PaymentPromiseStore& store,
                                             std::function<std::optional<std::string>()> local_account,
                                             AccountSigningKeyLookup account_key);

} // namespace pbr
