#include "domain/messaging/PaymentPromiseAvoid.h"

#include "common/PbrCompat.h"

namespace pbr {
namespace {

std::string CounterpartyAccountId(const PaymentPromise& promise, const std::string& local_account_id) {
  if (promise.payer_account_id == local_account_id) {
    return promise.payee_account_id;
  }
  if (promise.payee_account_id == local_account_id) {
    return promise.payer_account_id;
  }
  return {};
}

} // namespace

Roe<void> PaymentPromiseAvoid::AvoidCounterparty(PaymentPromiseStore& promises, IContactTrustAccess& contacts,
                                                 const std::string& promise_id,
                                                 const std::string& local_account_id) {
  if (local_account_id.empty()) {
    return Error("local_account_id required");
  }
  auto loaded = promises.Get(promise_id);
  if (!loaded) {
    return loaded.error();
  }
  if (!loaded->has_value()) {
    return Error("payment promise not found");
  }
  const PaymentPromise& promise = **loaded;
  const std::string other = CounterpartyAccountId(promise, local_account_id);
  if (other.empty()) {
    return Error("local_account_id is not a party to this promise");
  }

  auto marked = promises.MarkLocalAvoid(promise_id, true);
  if (!marked) {
    return marked.error();
  }

  return contacts.BlockAccountIfPresent(other);
}

bool PaymentPromiseAvoid::ShouldAvoid(const PaymentPromiseStore& promises, const IContactTrustAccess& contacts,
                                      const std::string& local_account_id, const std::string& other_account_id) {
  if (other_account_id.empty()) {
    return false;
  }
  if (promises.HasLocalAvoidAgainst(local_account_id, other_account_id)) {
    return true;
  }
  auto blocked = contacts.IsAccountBlocked(other_account_id);
  return blocked && *blocked;
}

} // namespace pbr
