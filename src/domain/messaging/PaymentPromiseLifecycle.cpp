#include "domain/messaging/PaymentPromiseLifecycle.h"

#include "domain/messaging/PaymentPromiseAvoid.h"
#include "domain/messaging/PaymentPromiseCodec.h"

#include "common/Utilities.h"

#include "common/PbrCompat.h"

namespace pbr {
namespace {

Roe<PaymentPromise> LoadRequired(PaymentPromiseStore& store, const std::string& promise_id) {
  auto loaded = store.Get(promise_id);
  if (!loaded) {
    return loaded.error();
  }
  if (!loaded->has_value()) {
    return Error("payment promise not found");
  }
  return **loaded;
}

Roe<std::string> RequireAccountId(IdentityStore& identity) {
  auto account_id = identity.GetAccountId();
  if (!account_id) {
    return account_id.error();
  }
  if (account_id->empty()) {
    return Error("account id missing");
  }
  return *account_id;
}

Roe<ByteVector> RequireAccountSecret(IdentityStore& identity) {
  auto secret = identity.GetAccountMlDsaPrivateKey();
  if (!secret) {
    return secret.error();
  }
  return *secret;
}

bool IsParty(const PaymentPromise& promise, const std::string& account_id) {
  return promise.payer_account_id == account_id || promise.payee_account_id == account_id;
}

} // namespace

Roe<PaymentPromise> PaymentPromiseLifecycle::CreateOffer(PaymentPromiseStore& store, IdentityStore& identity,
                                                         const OfferParams& params) {
  if (params.counterparty_account_id.empty()) {
    return Error("counterparty_account_id required");
  }
  if (params.amount_minor < 0) {
    return Error("amount_minor must be >= 0");
  }
  auto local_account = RequireAccountId(identity);
  if (!local_account) {
    return local_account.error();
  }
  if (*local_account == params.counterparty_account_id) {
    return Error("counterparty must differ from local account");
  }
  auto secret = RequireAccountSecret(identity);
  if (!secret) {
    return secret.error();
  }

  PaymentPromise promise;
  promise.promise_id = std::string("promise:") + util::GenerateUuid();
  if (params.local_is_payer) {
    promise.payer_account_id = *local_account;
    promise.payee_account_id = params.counterparty_account_id;
  } else {
    promise.payer_account_id = params.counterparty_account_id;
    promise.payee_account_id = *local_account;
  }
  promise.amount_minor = params.amount_minor;
  promise.currency = kPricingCurrencyId;
  promise.service_ref = params.service_ref;
  promise.terms_hash_b64 = params.terms_hash_b64;
  promise.content_commitment_b64 = params.content_commitment_b64;
  promise.release_rule = params.release_rule;
  promise.created_at_ms = util::NowUnixMs();
  promise.expires_at_ms = params.expires_at_ms;
  promise.state = PaymentPromiseState::Offered;

  auto signature = PaymentPromiseCodec::SignPromise(*secret, promise);
  if (!signature) {
    return signature.error();
  }
  if (params.local_is_payer) {
    promise.payer_signature_b64 = *signature;
  } else {
    promise.payee_signature_b64 = *signature;
  }

  auto saved = store.Upsert(promise);
  if (!saved) {
    return saved.error();
  }
  return promise;
}

Roe<PaymentPromise> PaymentPromiseLifecycle::Accept(PaymentPromiseStore& store, IdentityStore& identity,
                                                    const std::string& promise_id) {
  auto promise = LoadRequired(store, promise_id);
  if (!promise) {
    return promise.error();
  }
  if (promise->state != PaymentPromiseState::Offered) {
    return Error("only Offered promises can be accepted");
  }
  auto local_account = RequireAccountId(identity);
  if (!local_account) {
    return local_account.error();
  }
  if (!IsParty(*promise, *local_account)) {
    return Error("local account is not a party to this promise");
  }
  auto secret = RequireAccountSecret(identity);
  if (!secret) {
    return secret.error();
  }

  // Accepting party signs the promise commitment if they have not already.
  const bool local_is_payer = promise->payer_account_id == *local_account;
  std::string& slot = local_is_payer ? promise->payer_signature_b64 : promise->payee_signature_b64;
  if (slot.empty()) {
    auto signature = PaymentPromiseCodec::SignPromise(*secret, *promise);
    if (!signature) {
      return signature.error();
    }
    slot = *signature;
  }
  promise->state = PaymentPromiseState::Accepted;
  auto saved = store.Upsert(*promise);
  if (!saved) {
    return saved.error();
  }
  return *promise;
}

Roe<PaymentPromise> PaymentPromiseLifecycle::MarkDelivering(PaymentPromiseStore& store,
                                                            const std::string& promise_id) {
  auto promise = LoadRequired(store, promise_id);
  if (!promise) {
    return promise.error();
  }
  if (promise->state != PaymentPromiseState::Accepted && promise->state != PaymentPromiseState::Delivering) {
    return Error("only Accepted (or already Delivering) promises can mark delivering");
  }
  promise->state = PaymentPromiseState::Delivering;
  auto saved = store.Upsert(*promise);
  if (!saved) {
    return saved.error();
  }
  return *promise;
}

Roe<PaymentPromise> PaymentPromiseLifecycle::RecordOutcome(PaymentPromiseStore& store, IdentityStore& identity,
                                                           const std::string& promise_id,
                                                           const PaymentPromiseState outcome,
                                                           const std::string& note) {
  if (!PaymentPromiseStateIsTerminal(outcome)) {
    return Error("outcome must be a terminal PaymentPromiseState");
  }
  auto promise = LoadRequired(store, promise_id);
  if (!promise) {
    return promise.error();
  }
  if (PaymentPromiseStateIsTerminal(promise->state)) {
    return Error("promise already has a terminal outcome");
  }
  auto local_account = RequireAccountId(identity);
  if (!local_account) {
    return local_account.error();
  }
  if (!IsParty(*promise, *local_account)) {
    return Error("local account is not a party to this promise");
  }
  auto secret = RequireAccountSecret(identity);
  if (!secret) {
    return secret.error();
  }

  promise->state = outcome;
  promise->outcome_actor_account_id = *local_account;
  promise->outcome_at_ms = util::NowUnixMs();
  promise->outcome_note = note;
  auto signature = PaymentPromiseCodec::SignOutcome(*secret, *promise);
  if (!signature) {
    return signature.error();
  }
  promise->outcome_signature_b64 = *signature;
  auto saved = store.Upsert(*promise);
  if (!saved) {
    return saved.error();
  }
  return *promise;
}

Roe<void> PaymentPromiseLifecycle::AvoidCounterparty(PaymentPromiseStore& store, ContactsStore& contacts,
                                                     IdentityStore& identity, const std::string& promise_id) {
  auto local_account = RequireAccountId(identity);
  if (!local_account) {
    return local_account.error();
  }
  return PaymentPromiseAvoid::AvoidCounterparty(store, contacts, promise_id, *local_account);
}

} // namespace pbr
