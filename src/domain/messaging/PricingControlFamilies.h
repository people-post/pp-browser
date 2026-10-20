#pragma once

#include "common/chat/SystemControl.h"

#include <cstdint>
#include <functional>
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
 * Payment-promise offers as a system-control family: an inbound offer is staged (committing it
 * takes the user's accept); the message stays in the chat. Only a party to the promise may send
 * it. Signatures are not checked yet.
 */
SystemControlFamily MakePaymentPromiseFamily(PaymentPromiseStore& store);

} // namespace pbr
