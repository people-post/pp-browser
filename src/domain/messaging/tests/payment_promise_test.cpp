#include "domain/messaging/PaymentPromiseAvoid.h"
#include "domain/messaging/PaymentPromiseCodec.h"
#include "domain/messaging/PaymentPromiseStore.h"
#include "foundation/crypto/MlDsa.h"
#include "domain/people/ContactsStore.h"
#include "domain/people/ContactTypes.h"

#include <filesystem>
#include <gtest/gtest.h>
#include "common/PbrCompat.h"

namespace {

using namespace pbr;

class PaymentPromiseTest : public ::testing::Test {
protected:
  void SetUp() override {
    data_dir_ = std::filesystem::temp_directory_path() /
                ("pp_payment_promise_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" +
                 std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(data_dir_);
    std::filesystem::create_directories(data_dir_);
  }

  void TearDown() override { std::filesystem::remove_all(data_dir_); }

  PaymentPromise SamplePromise() const {
    PaymentPromise promise;
    promise.promise_id = "promise:test-1";
    promise.payer_account_id = "account:alice";
    promise.payee_account_id = "account:bob";
    promise.amount_minor = 42;
    promise.currency = kPricingCurrencyId;
    promise.service_ref = "service:job-1";
    promise.terms_hash_b64 = "dGVybXM=";
    promise.release_rule = PaymentPromiseReleaseRule::PayerAck;
    promise.created_at_ms = 1700000000000;
    promise.expires_at_ms = 1700000360000;
    promise.state = PaymentPromiseState::Offered;
    return promise;
  }

  std::filesystem::path data_dir_;
};

TEST_F(PaymentPromiseTest, CodecRoundTripAndSignatures) {
  auto keys = MlDsa::GenerateKeyPair();
  ASSERT_TRUE(static_cast<bool>(keys)) << keys.error().message;

  PaymentPromise promise = SamplePromise();
  auto payer_sig = PaymentPromiseCodec::SignPromise(keys->secret_key, promise);
  ASSERT_TRUE(static_cast<bool>(payer_sig)) << payer_sig.error().message;
  promise.payer_signature_b64 = *payer_sig;

  auto encoded = PaymentPromiseCodec::Encode(promise);
  ASSERT_TRUE(static_cast<bool>(encoded)) << encoded.error().message;
  auto decoded = PaymentPromiseCodec::Decode(*encoded);
  ASSERT_TRUE(static_cast<bool>(decoded)) << decoded.error().message;
  EXPECT_EQ(decoded->promise_id, promise.promise_id);
  EXPECT_EQ(decoded->amount_minor, 42);
  EXPECT_EQ(decoded->payer_signature_b64, promise.payer_signature_b64);

  auto ok = PaymentPromiseCodec::VerifyPromiseSignature(keys->public_key, *decoded, decoded->payer_signature_b64);
  ASSERT_TRUE(static_cast<bool>(ok)) << ok.error().message;
  EXPECT_TRUE(*ok);

  decoded->state = PaymentPromiseState::Released;
  decoded->outcome_actor_account_id = promise.payer_account_id;
  decoded->outcome_at_ms = promise.created_at_ms + 1;
  decoded->outcome_note = "ok";
  auto outcome_sig = PaymentPromiseCodec::SignOutcome(keys->secret_key, *decoded);
  ASSERT_TRUE(static_cast<bool>(outcome_sig)) << outcome_sig.error().message;
  decoded->outcome_signature_b64 = *outcome_sig;
  auto outcome_ok = PaymentPromiseCodec::VerifyOutcomeSignature(keys->public_key, *decoded);
  ASSERT_TRUE(static_cast<bool>(outcome_ok)) << outcome_ok.error().message;
  EXPECT_TRUE(*outcome_ok);

  // Tamper with amount — promise signature must fail.
  decoded->amount_minor = 99;
  auto bad = PaymentPromiseCodec::VerifyPromiseSignature(keys->public_key, *decoded, decoded->payer_signature_b64);
  ASSERT_TRUE(static_cast<bool>(bad)) << bad.error().message;
  EXPECT_FALSE(*bad);
}

TEST_F(PaymentPromiseTest, StorePersistsAndLocalAvoid) {
  {
    PaymentPromiseStore store(data_dir_.string());
    ASSERT_TRUE(static_cast<bool>(store.Load()));
    ASSERT_TRUE(static_cast<bool>(store.Upsert(SamplePromise())));
    ASSERT_TRUE(static_cast<bool>(store.MarkLocalAvoid("promise:test-1", true)));
  }
  PaymentPromiseStore reloaded(data_dir_.string());
  ASSERT_TRUE(static_cast<bool>(reloaded.Load()));
  auto got = reloaded.Get("promise:test-1");
  ASSERT_TRUE(static_cast<bool>(got));
  ASSERT_TRUE(got->has_value());
  EXPECT_TRUE((*got)->local_avoid);
  EXPECT_TRUE(reloaded.HasLocalAvoidAgainst("account:alice", "account:bob"));
  EXPECT_FALSE(reloaded.HasLocalAvoidAgainst("account:alice", "account:carol"));
}

TEST_F(PaymentPromiseTest, AvoidBlocksContactAndStampsReceipt) {
  ContactsStore contacts(data_dir_.string());
  Contact contact;
  contact.id = "contact-bob";
  contact.local.display_name = "Bob";
  contact.remote.ids = {{ContactIdKind::Account, "account:bob", true}};
  contact.local.trust = TrustLevel::Unknown;
  SyncContactMirrors(contact);
  ASSERT_TRUE(static_cast<bool>(contacts.Upsert(contact)));

  PaymentPromiseStore promises(data_dir_.string());
  ASSERT_TRUE(static_cast<bool>(promises.Load()));
  ASSERT_TRUE(static_cast<bool>(promises.Upsert(SamplePromise())));

  auto avoided =
      PaymentPromiseAvoid::AvoidCounterparty(promises, contacts, "promise:test-1", "account:alice");
  ASSERT_TRUE(static_cast<bool>(avoided)) << avoided.error().message;
  EXPECT_TRUE(PaymentPromiseAvoid::ShouldAvoid(promises, contacts, "account:alice", "account:bob"));

  auto hit = contacts.FindByIdentity("account:bob", ContactIdKind::Account);
  ASSERT_TRUE(static_cast<bool>(hit));
  ASSERT_TRUE(hit->has_value());
  EXPECT_EQ((*hit)->local.trust, TrustLevel::Blocked);
  EXPECT_EQ((*hit)->trust, TrustLevel::Blocked);

  auto got = promises.Get("promise:test-1");
  ASSERT_TRUE(static_cast<bool>(got));
  ASSERT_TRUE(got->has_value());
  EXPECT_TRUE((*got)->local_avoid);
}

TEST_F(PaymentPromiseTest, PendingInboundAcceptAndIgnore) {
  PaymentPromiseStore store(data_dir_.string());
  ASSERT_TRUE(static_cast<bool>(store.Load()));

  PaymentPromise remote = SamplePromise();
  remote.promise_id = "promise:inbound-1";
  ASSERT_TRUE(static_cast<bool>(store.StageInbound(remote))) << "stage failed";

  auto committed = store.Get("promise:inbound-1");
  ASSERT_TRUE(static_cast<bool>(committed));
  EXPECT_FALSE(committed->has_value());

  auto pending = store.GetPendingInbound("promise:inbound-1");
  ASSERT_TRUE(static_cast<bool>(pending));
  ASSERT_TRUE(pending->has_value());
  EXPECT_EQ((*pending)->amount_minor, 42);

  auto accepted = store.AcceptInbound("promise:inbound-1");
  ASSERT_TRUE(static_cast<bool>(accepted)) << accepted.error().message;
  EXPECT_EQ(accepted->promise_id, "promise:inbound-1");

  auto after_accept_pending = store.GetPendingInbound("promise:inbound-1");
  ASSERT_TRUE(static_cast<bool>(after_accept_pending));
  EXPECT_FALSE(after_accept_pending->has_value());
  auto after_accept_committed = store.Get("promise:inbound-1");
  ASSERT_TRUE(static_cast<bool>(after_accept_committed));
  ASSERT_TRUE(after_accept_committed->has_value());

  PaymentPromise ignored = SamplePromise();
  ignored.promise_id = "promise:inbound-2";
  ASSERT_TRUE(static_cast<bool>(store.StageInbound(ignored)));
  auto drop = store.IgnoreInbound("promise:inbound-2");
  ASSERT_TRUE(static_cast<bool>(drop));
  EXPECT_TRUE(*drop);
  auto still_pending = store.GetPendingInbound("promise:inbound-2");
  ASSERT_TRUE(static_cast<bool>(still_pending));
  EXPECT_FALSE(still_pending->has_value());
  auto not_committed = store.Get("promise:inbound-2");
  ASSERT_TRUE(static_cast<bool>(not_committed));
  EXPECT_FALSE(not_committed->has_value());

  // Reload persists pending + committed split.
  {
    PaymentPromiseStore again(data_dir_.string());
    ASSERT_TRUE(static_cast<bool>(again.Load()));
    auto listed_pending = again.ListPendingInbound();
    ASSERT_TRUE(static_cast<bool>(listed_pending));
    EXPECT_TRUE(listed_pending->empty());
    auto listed = again.Get("promise:inbound-1");
    ASSERT_TRUE(static_cast<bool>(listed));
    ASSERT_TRUE(listed->has_value());
  }
}

} // namespace
