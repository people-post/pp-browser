#include "domain/messaging/PaymentPromiseAvoid.h"

#include "domain/people/ContactTypes.h"

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

Roe<void> PaymentPromiseAvoid::AvoidCounterparty(PaymentPromiseStore& promises, ContactsStore& contacts,
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

  auto hit = contacts.FindByIdentity(other, ContactIdKind::Account);
  if (!hit) {
    return hit.error();
  }
  if (!hit->has_value()) {
    // Receipt stamp is enough when the address book has no row yet.
    return {};
  }
  Contact contact = **hit;
  contact.local.trust = TrustLevel::Blocked;
  SyncContactMirrors(contact);
  auto upserted = contacts.Upsert(contact);
  if (!upserted) {
    return upserted.error();
  }
  return {};
}

bool PaymentPromiseAvoid::ShouldAvoid(const PaymentPromiseStore& promises, const ContactsStore& contacts,
                                      const std::string& local_account_id, const std::string& other_account_id) {
  if (other_account_id.empty()) {
    return false;
  }
  if (promises.HasLocalAvoidAgainst(local_account_id, other_account_id)) {
    return true;
  }
  auto hit = contacts.FindByIdentity(other_account_id, ContactIdKind::Account);
  if (!hit || !hit->has_value()) {
    return false;
  }
  return (*hit)->local.trust == TrustLevel::Blocked || (*hit)->trust == TrustLevel::Blocked;
}

} // namespace pbr
