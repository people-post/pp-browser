#pragma once

#include "foundation/crypto/CryptoTypes.h"
#include "foundation/data/PaymentPromiseTypes.h"
#include "common/Error.h"

#include <functional>
#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** An account's ML-DSA-65 signing public key, as the directory publishes it. */
using AccountSigningKeyLookup = std::function<Roe<ByteVector>(const std::string& account_id)>;

/**
 * What a promise received from the counterparty may change. Promises are two-party, so the
 * counterparty is the party that is not `local_account`; its signature must verify on what it
 * sends, and the signed terms (parties, amount, refs, times) never change after the offer.
 *
 * - Offered: a new promise (none committed under its id), signed by the counterparty.
 * - Accepted: our Offered promise, countersigned by the counterparty.
 * - Terminal (released, rejected, …): our open promise, with an outcome the counterparty signed
 *   as its actor.
 *
 * Returns the row to commit: our copy with only the counterparty's part taken from `incoming`.
 */
Roe<PaymentPromise> MergeInboundPaymentPromise(const std::optional<PaymentPromise>& committed,
                                               const PaymentPromise& incoming, const std::string& local_account,
                                               const ByteVector& counterparty_key);

/** The party of `promise` that is not `local_account`; none when `local_account` is not exactly one party. */
std::optional<std::string> PaymentPromiseCounterparty(const PaymentPromise& promise, const std::string& local_account);

} // namespace pbr
