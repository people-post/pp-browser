#include "domain/messaging/PaymentPromiseCodec.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"

#include "common/ValueJson.h"

#include <sstream>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

void AppendField(std::ostringstream& out, const char* key, const std::string& value) {
  out << key << '=' << value << '\n';
}

void AppendField(std::ostringstream& out, const char* key, const int64_t value) {
  out << key << '=' << value << '\n';
}

ByteVector ToBytes(const std::string& text) {
  return ByteVector(text.begin(), text.end());
}

Roe<std::string> SignBytes(const ByteVector& secret_key, const ByteVector& message) {
  auto signature = MlDsa::Sign(secret_key, message);
  if (!signature) {
    return signature.error();
  }
  return Base64Encode(*signature);
}

Roe<bool> VerifyBytes(const ByteVector& public_key, const ByteVector& message, const std::string& signature_b64) {
  auto signature = Base64Decode(signature_b64);
  if (!signature) {
    return signature.error();
  }
  return MlDsa::Verify(public_key, message, *signature);
}

Object PromiseToObject(const PaymentPromise& promise) {
  Object json;
  json.set("promise_id", promise.promise_id);
  json.set("payer_account_id", promise.payer_account_id);
  json.set("payee_account_id", promise.payee_account_id);
  json.set("amount_minor", promise.amount_minor);
  json.set("currency", promise.currency.empty() ? std::string(kPricingCurrencyId) : promise.currency);
  json.set("service_ref", promise.service_ref);
  json.set("terms_hash_b64", promise.terms_hash_b64);
  json.set("content_commitment_b64", promise.content_commitment_b64);
  json.set("release_rule", PaymentPromiseReleaseRuleToString(promise.release_rule));
  json.set("created_at_ms", promise.created_at_ms);
  json.set("expires_at_ms", promise.expires_at_ms);
  json.set("state", PaymentPromiseStateToString(promise.state));
  json.set("payer_signature_b64", promise.payer_signature_b64);
  json.set("payee_signature_b64", promise.payee_signature_b64);
  json.set("outcome_actor_account_id", promise.outcome_actor_account_id);
  json.set("outcome_signature_b64", promise.outcome_signature_b64);
  json.set("outcome_at_ms", promise.outcome_at_ms);
  json.set("outcome_note", promise.outcome_note);
  json.set("local_avoid", promise.local_avoid);
  return json;
}

PaymentPromise PromiseFromObject(const Object& json) {
  PaymentPromise promise;
  promise.promise_id = json.getString("promise_id").value_or("");
  promise.payer_account_id = json.getString("payer_account_id").value_or("");
  promise.payee_account_id = json.getString("payee_account_id").value_or("");
  promise.amount_minor = json.getIf<int64_t>("amount_minor").value_or(0);
  promise.currency = json.getString("currency").value_or(std::string(kPricingCurrencyId));
  promise.service_ref = json.getString("service_ref").value_or("");
  promise.terms_hash_b64 = json.getString("terms_hash_b64").value_or("");
  promise.content_commitment_b64 = json.getString("content_commitment_b64").value_or("");
  promise.release_rule =
      PaymentPromiseReleaseRuleFromString(json.getString("release_rule").value_or("payer_ack"));
  promise.created_at_ms = json.getIf<int64_t>("created_at_ms").value_or(0);
  promise.expires_at_ms = json.getIf<int64_t>("expires_at_ms").value_or(0);
  promise.state = PaymentPromiseStateFromString(json.getString("state").value_or("offered"));
  promise.payer_signature_b64 = json.getString("payer_signature_b64").value_or("");
  promise.payee_signature_b64 = json.getString("payee_signature_b64").value_or("");
  promise.outcome_actor_account_id = json.getString("outcome_actor_account_id").value_or("");
  promise.outcome_signature_b64 = json.getString("outcome_signature_b64").value_or("");
  promise.outcome_at_ms = json.getIf<int64_t>("outcome_at_ms").value_or(0);
  promise.outcome_note = json.getString("outcome_note").value_or("");
  promise.local_avoid = json.getIf<bool>("local_avoid").value_or(false);
  return promise;
}

} // namespace

Roe<std::string> PaymentPromiseCodec::Encode(const PaymentPromise& promise) {
  return DumpJson(PromiseToObject(promise));
}

Roe<PaymentPromise> PaymentPromiseCodec::Decode(const std::string& json) {
  auto root = TryParseObject(json);
  if (!root) {
    return Error("Invalid payment promise JSON");
  }
  PaymentPromise promise = PromiseFromObject(*root);
  if (promise.promise_id.empty()) {
    return Error("payment promise missing promise_id");
  }
  if (promise.payer_account_id.empty() || promise.payee_account_id.empty()) {
    return Error("payment promise missing payer/payee account id");
  }
  return promise;
}

Roe<ByteVector> PaymentPromiseCodec::BuildPromiseSignBytes(const PaymentPromise& promise) {
  // Domain-separated canonical text. Signatures / outcome / local_avoid excluded.
  std::ostringstream out;
  out << "pp-browser:payment-promise-sign-v1\n";
  AppendField(out, "promise_id", promise.promise_id);
  AppendField(out, "payer_account_id", promise.payer_account_id);
  AppendField(out, "payee_account_id", promise.payee_account_id);
  AppendField(out, "amount_minor", promise.amount_minor);
  AppendField(out, "currency", promise.currency.empty() ? kPricingCurrencyId : promise.currency);
  AppendField(out, "service_ref", promise.service_ref);
  AppendField(out, "terms_hash_b64", promise.terms_hash_b64);
  AppendField(out, "content_commitment_b64", promise.content_commitment_b64);
  AppendField(out, "release_rule", PaymentPromiseReleaseRuleToString(promise.release_rule));
  AppendField(out, "created_at_ms", promise.created_at_ms);
  AppendField(out, "expires_at_ms", promise.expires_at_ms);
  return ToBytes(out.str());
}

Roe<ByteVector> PaymentPromiseCodec::BuildOutcomeSignBytes(const PaymentPromise& promise) {
  std::ostringstream out;
  out << "pp-browser:payment-promise-outcome-sign-v1\n";
  AppendField(out, "promise_id", promise.promise_id);
  AppendField(out, "state", PaymentPromiseStateToString(promise.state));
  AppendField(out, "outcome_actor_account_id", promise.outcome_actor_account_id);
  AppendField(out, "outcome_at_ms", promise.outcome_at_ms);
  AppendField(out, "outcome_note", promise.outcome_note);
  AppendField(out, "content_commitment_b64", promise.content_commitment_b64);
  return ToBytes(out.str());
}

Roe<std::string> PaymentPromiseCodec::SignPromise(const ByteVector& secret_key, const PaymentPromise& promise) {
  auto bytes = BuildPromiseSignBytes(promise);
  if (!bytes) {
    return bytes.error();
  }
  return SignBytes(secret_key, *bytes);
}

Roe<std::string> PaymentPromiseCodec::SignPromise(IAccountSigningAccess& identity, const PaymentPromise& promise) {
  auto bytes = BuildPromiseSignBytes(promise);
  if (!bytes) {
    return bytes.error();
  }
  return identity.SignAccountBytes(*bytes);
}

Roe<bool> PaymentPromiseCodec::VerifyPromiseSignature(const ByteVector& public_key, const PaymentPromise& promise,
                                                      const std::string& signature_b64) {
  if (signature_b64.empty()) {
    return false;
  }
  auto bytes = BuildPromiseSignBytes(promise);
  if (!bytes) {
    return bytes.error();
  }
  return VerifyBytes(public_key, *bytes, signature_b64);
}

Roe<std::string> PaymentPromiseCodec::SignOutcome(const ByteVector& secret_key, const PaymentPromise& promise) {
  if (!PaymentPromiseStateIsTerminal(promise.state)) {
    return Error("outcome signature requires a terminal promise state");
  }
  if (promise.outcome_actor_account_id.empty()) {
    return Error("outcome signature requires outcome_actor_account_id");
  }
  auto bytes = BuildOutcomeSignBytes(promise);
  if (!bytes) {
    return bytes.error();
  }
  return SignBytes(secret_key, *bytes);
}

Roe<std::string> PaymentPromiseCodec::SignOutcome(IAccountSigningAccess& identity, const PaymentPromise& promise) {
  if (!PaymentPromiseStateIsTerminal(promise.state)) {
    return Error("outcome signature requires a terminal promise state");
  }
  if (promise.outcome_actor_account_id.empty()) {
    return Error("outcome signature requires outcome_actor_account_id");
  }
  auto bytes = BuildOutcomeSignBytes(promise);
  if (!bytes) {
    return bytes.error();
  }
  return identity.SignAccountBytes(*bytes);
}

Roe<bool> PaymentPromiseCodec::VerifyOutcomeSignature(const ByteVector& public_key, const PaymentPromise& promise) {
  if (promise.outcome_signature_b64.empty()) {
    return false;
  }
  if (!PaymentPromiseStateIsTerminal(promise.state)) {
    return false;
  }
  auto bytes = BuildOutcomeSignBytes(promise);
  if (!bytes) {
    return bytes.error();
  }
  return VerifyBytes(public_key, *bytes, promise.outcome_signature_b64);
}

} // namespace pbr
