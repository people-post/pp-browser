#include "domain/pricing/PaymentPromiseInbound.h"

#include "domain/pricing/PaymentPromiseCodec.h"
#include "foundation/crypto/MlDsa.h"

#include <gtest/gtest.h>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

constexpr const char* kAlice = "account:alice"; // local, the payer
constexpr const char* kBob = "account:bob";     // counterparty, the payee

class PaymentPromiseInboundTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto alice = MlDsa::GenerateKeyPair();
    auto bob = MlDsa::GenerateKeyPair();
    ASSERT_TRUE(alice && bob);
    alice_ = *alice;
    bob_ = *bob;
  }

  static PaymentPromise Terms() {
    PaymentPromise promise;
    promise.promise_id = "promise:inbound";
    promise.payer_account_id = kAlice;
    promise.payee_account_id = kBob;
    promise.amount_minor = 42;
    promise.service_ref = "thread:t-1";
    promise.created_at_ms = 1700000000000;
    promise.state = PaymentPromiseState::Offered;
    return promise;
  }

  /** Our Offered row: signed by us as payer. */
  PaymentPromise OurOffer() const {
    PaymentPromise promise = Terms();
    promise.payer_signature_b64 = *PaymentPromiseCodec::SignPromise(alice_.secret_key, promise);
    return promise;
  }

  /** Bob's offer to us: signed by Bob as payee. */
  PaymentPromise TheirOffer() const {
    PaymentPromise promise = Terms();
    promise.payee_signature_b64 = *PaymentPromiseCodec::SignPromise(bob_.secret_key, promise);
    return promise;
  }

  PaymentPromise TheirOutcome(const PaymentPromise& base, PaymentPromiseState state) const {
    PaymentPromise promise = base;
    promise.state = state;
    promise.outcome_actor_account_id = kBob;
    promise.outcome_at_ms = 1700000100000;
    promise.outcome_signature_b64 = *PaymentPromiseCodec::SignOutcome(bob_.secret_key, promise);
    return promise;
  }

  MlDsaKeyPair alice_;
  MlDsaKeyPair bob_;
};

TEST_F(PaymentPromiseInboundTest, NewOfferSignedByTheCounterparty) {
  PaymentPromise offer = TheirOffer();
  offer.payer_signature_b64 = "forged-for-us";
  offer.local_avoid = true;
  auto row = MergeInboundPaymentPromise(std::nullopt, offer, kAlice, bob_.public_key);
  ASSERT_TRUE(row) << row.error().message;
  EXPECT_EQ(row->state, PaymentPromiseState::Offered);
  EXPECT_TRUE(row->payer_signature_b64.empty()) << "only we sign our slot";
  EXPECT_FALSE(row->local_avoid);
}

TEST_F(PaymentPromiseInboundTest, OfferRefusedWhenUnsignedOrSignedByAnotherKey) {
  PaymentPromise unsigned_offer = Terms();
  EXPECT_FALSE(MergeInboundPaymentPromise(std::nullopt, unsigned_offer, kAlice, bob_.public_key));
  PaymentPromise wrong_signer = Terms();
  wrong_signer.payee_signature_b64 = *PaymentPromiseCodec::SignPromise(alice_.secret_key, wrong_signer);
  EXPECT_FALSE(MergeInboundPaymentPromise(std::nullopt, wrong_signer, kAlice, bob_.public_key));
}

TEST_F(PaymentPromiseInboundTest, OfferNeverReplacesAPromiseWeHave) {
  EXPECT_FALSE(MergeInboundPaymentPromise(OurOffer(), TheirOffer(), kAlice, bob_.public_key));
}

TEST_F(PaymentPromiseInboundTest, OfferNotInvolvingUsIsRefused) {
  PaymentPromise other = TheirOffer();
  EXPECT_FALSE(MergeInboundPaymentPromise(std::nullopt, other, "account:carol", bob_.public_key));
}

TEST_F(PaymentPromiseInboundTest, AcceptCountersignsOurOffer) {
  const PaymentPromise ours = OurOffer();
  PaymentPromise accept = ours;
  accept.state = PaymentPromiseState::Accepted;
  accept.payer_signature_b64 = "theirs-to-claim";
  accept.payee_signature_b64 = *PaymentPromiseCodec::SignPromise(bob_.secret_key, accept);
  auto row = MergeInboundPaymentPromise(ours, accept, kAlice, bob_.public_key);
  ASSERT_TRUE(row) << row.error().message;
  EXPECT_EQ(row->state, PaymentPromiseState::Accepted);
  EXPECT_EQ(row->payee_signature_b64, accept.payee_signature_b64);
  EXPECT_EQ(row->payer_signature_b64, ours.payer_signature_b64) << "our signature stays ours";
}

TEST_F(PaymentPromiseInboundTest, AcceptCannotChangeTheTerms) {
  PaymentPromise accept = OurOffer();
  accept.state = PaymentPromiseState::Accepted;
  accept.amount_minor = 1;
  accept.payee_signature_b64 = *PaymentPromiseCodec::SignPromise(bob_.secret_key, accept);
  EXPECT_FALSE(MergeInboundPaymentPromise(OurOffer(), accept, kAlice, bob_.public_key));
}

TEST_F(PaymentPromiseInboundTest, AcceptOnlyFromOffered) {
  PaymentPromise ours = OurOffer();
  ours.state = PaymentPromiseState::Accepted;
  PaymentPromise accept = OurOffer();
  accept.state = PaymentPromiseState::Accepted;
  accept.payee_signature_b64 = *PaymentPromiseCodec::SignPromise(bob_.secret_key, accept);
  EXPECT_FALSE(MergeInboundPaymentPromise(ours, accept, kAlice, bob_.public_key));
  EXPECT_FALSE(MergeInboundPaymentPromise(std::nullopt, accept, kAlice, bob_.public_key));
}

TEST_F(PaymentPromiseInboundTest, OfferedCannotGoBack) {
  PaymentPromise ours = OurOffer();
  ours.state = PaymentPromiseState::Accepted;
  EXPECT_FALSE(MergeInboundPaymentPromise(ours, TheirOffer(), kAlice, bob_.public_key));
}

TEST_F(PaymentPromiseInboundTest, OutcomeSignedByTheCounterpartyAsActor) {
  PaymentPromise ours = OurOffer();
  ours.state = PaymentPromiseState::Accepted;
  auto row = MergeInboundPaymentPromise(ours, TheirOutcome(ours, PaymentPromiseState::Rejected), kAlice,
                                        bob_.public_key);
  ASSERT_TRUE(row) << row.error().message;
  EXPECT_EQ(row->state, PaymentPromiseState::Rejected);
  EXPECT_EQ(row->outcome_actor_account_id, kBob);
}

TEST_F(PaymentPromiseInboundTest, OutcomeRefusedWhenClaimedForUsOrAlreadyFinal) {
  PaymentPromise ours = OurOffer();
  ours.state = PaymentPromiseState::Accepted;
  PaymentPromise as_us = TheirOutcome(ours, PaymentPromiseState::Released);
  as_us.outcome_actor_account_id = kAlice;
  EXPECT_FALSE(MergeInboundPaymentPromise(ours, as_us, kAlice, bob_.public_key));

  PaymentPromise final_row = ours;
  final_row.state = PaymentPromiseState::Released;
  EXPECT_FALSE(MergeInboundPaymentPromise(final_row, TheirOutcome(ours, PaymentPromiseState::Rejected), kAlice,
                                          bob_.public_key));
}

TEST_F(PaymentPromiseInboundTest, OutcomeWithABadSignatureIsRefused) {
  PaymentPromise ours = OurOffer();
  ours.state = PaymentPromiseState::Accepted;
  PaymentPromise outcome = TheirOutcome(ours, PaymentPromiseState::Rejected);
  outcome.outcome_note = "changed after signing";
  EXPECT_FALSE(MergeInboundPaymentPromise(ours, outcome, kAlice, bob_.public_key));
}

} // namespace
} // namespace pbr
