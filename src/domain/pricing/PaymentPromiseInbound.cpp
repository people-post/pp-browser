#include "domain/pricing/PaymentPromiseInbound.h"

#include "domain/pricing/PaymentPromiseCodec.h"

#include <utility>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

Roe<void> RequireSameTerms(const PaymentPromise& committed, const PaymentPromise& incoming) {
  auto ours = PaymentPromiseCodec::BuildPromiseSignBytes(committed);
  if (!ours) {
    return ours.error();
  }
  auto theirs = PaymentPromiseCodec::BuildPromiseSignBytes(incoming);
  if (!theirs) {
    return theirs.error();
  }
  if (*ours != *theirs) {
    return Error("payment promise terms changed");
  }
  return {};
}

Roe<void> RequirePromiseSignature(const ByteVector& key, const PaymentPromise& promise, const std::string& signature) {
  auto verified = PaymentPromiseCodec::VerifyPromiseSignature(key, promise, signature);
  if (!verified) {
    return verified.error();
  }
  if (!*verified) {
    return Error("payment promise signature does not verify");
  }
  return {};
}

} // namespace

std::optional<std::string> PaymentPromiseCounterparty(const PaymentPromise& promise, const std::string& local_account) {
  if (local_account.empty() || promise.payer_account_id == promise.payee_account_id) {
    return std::nullopt;
  }
  if (promise.payer_account_id == local_account) {
    return promise.payee_account_id;
  }
  if (promise.payee_account_id == local_account) {
    return promise.payer_account_id;
  }
  return std::nullopt;
}

Roe<PaymentPromise> MergeInboundPaymentPromise(const std::optional<PaymentPromise>& committed,
                                               const PaymentPromise& incoming, const std::string& local_account,
                                               const ByteVector& counterparty_key) {
  const auto counterparty = PaymentPromiseCounterparty(incoming, local_account);
  if (!counterparty) {
    return Error("payment promise is not between us and one counterparty");
  }
  const bool counterparty_is_payer = incoming.payer_account_id == *counterparty;
  const std::string& their_signature =
      counterparty_is_payer ? incoming.payer_signature_b64 : incoming.payee_signature_b64;

  if (incoming.state == PaymentPromiseState::Offered) {
    if (committed) {
      return Error("payment promise already exists");
    }
    if (auto signed_ok = RequirePromiseSignature(counterparty_key, incoming, their_signature); !signed_ok) {
      return signed_ok.error();
    }
    PaymentPromise row = incoming;
    // Ours only when we sign it; outcomes and local marks are never theirs to set.
    (counterparty_is_payer ? row.payee_signature_b64 : row.payer_signature_b64).clear();
    row.outcome_actor_account_id.clear();
    row.outcome_signature_b64.clear();
    row.outcome_at_ms = 0;
    row.outcome_note.clear();
    row.local_avoid = false;
    return row;
  }

  if (!committed) {
    return Error("payment promise not found");
  }
  if (auto same = RequireSameTerms(*committed, incoming); !same) {
    return same.error();
  }
  PaymentPromise row = *committed;

  if (incoming.state == PaymentPromiseState::Accepted) {
    if (committed->state != PaymentPromiseState::Offered) {
      return Error("only an offered payment promise can be accepted");
    }
    if (auto signed_ok = RequirePromiseSignature(counterparty_key, incoming, their_signature); !signed_ok) {
      return signed_ok.error();
    }
    (counterparty_is_payer ? row.payer_signature_b64 : row.payee_signature_b64) = their_signature;
    row.state = PaymentPromiseState::Accepted;
    return row;
  }

  if (PaymentPromiseStateIsTerminal(incoming.state)) {
    if (PaymentPromiseStateIsTerminal(committed->state)) {
      return Error("payment promise already has an outcome");
    }
    if (incoming.outcome_actor_account_id != *counterparty) {
      return Error("payment promise outcome not by the counterparty");
    }
    auto verified = PaymentPromiseCodec::VerifyOutcomeSignature(counterparty_key, incoming);
    if (!verified) {
      return verified.error();
    }
    if (!*verified) {
      return Error("payment promise outcome signature does not verify");
    }
    row.state = incoming.state;
    row.outcome_actor_account_id = incoming.outcome_actor_account_id;
    row.outcome_signature_b64 = incoming.outcome_signature_b64;
    row.outcome_at_ms = incoming.outcome_at_ms;
    row.outcome_note = incoming.outcome_note;
    return row;
  }

  return Error("payment promise state cannot be received");
}

} // namespace pbr
