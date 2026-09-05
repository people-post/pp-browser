#include "domain/messaging/PaymentPromiseLifecycle.h"
#include "domain/messaging/PaymentPromiseCodec.h"
#include "domain/messaging/PaymentPromiseStore.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "domain/people/ContactsStore.h"
#include "domain/people/ContactTypes.h"
#include "domain/people/IdentityStore.h"

#include <filesystem>
#include <gtest/gtest.h>
#include "common/PbrCompat.h"

namespace {

using namespace pbr;

ByteVector MakeTestDek() {
  EnsureSodiumInit();
  ByteVector dek(kDataEncryptionKeySize);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(i + 7);
  }
  return dek;
}

class PaymentPromiseLifecycleTest : public ::testing::Test {
protected:
  void SetUp() override {
    data_dir_ = std::filesystem::temp_directory_path() /
                ("pp_payment_promise_lifecycle_" +
                 std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" +
                 std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(data_dir_);
    std::filesystem::create_directories(data_dir_);

    identity_ = std::make_unique<IdentityStore>(data_dir_.string(), "lifecycle-profile");
    ASSERT_TRUE(identity_->SetDek(MakeTestDek()));
    auto created = identity_->LoadOrCreate();
    ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
    local_account_id_ = created->account_id;
    ASSERT_FALSE(local_account_id_.empty());
    account_public_key_b64_ = created->account_signing_public_key_b64;
    ASSERT_FALSE(account_public_key_b64_.empty());

    store_ = std::make_unique<PaymentPromiseStore>(data_dir_.string());
    ASSERT_TRUE(static_cast<bool>(store_->Load()));
  }

  void TearDown() override {
    store_.reset();
    identity_.reset();
    std::filesystem::remove_all(data_dir_);
  }

  std::filesystem::path data_dir_;
  std::unique_ptr<IdentityStore> identity_;
  std::unique_ptr<PaymentPromiseStore> store_;
  std::string local_account_id_;
  std::string account_public_key_b64_;
};

TEST_F(PaymentPromiseLifecycleTest, CreateAcceptReleaseRoundTrip) {
  PaymentPromiseLifecycle::OfferParams params;
  params.counterparty_account_id = "account:bob-counterparty";
  params.local_is_payer = true;
  params.amount_minor = 25;
  params.service_ref = "service:job-lifecycle";
  params.terms_hash_b64 = "dGVybXM=";

  auto offered = PaymentPromiseLifecycle::CreateOffer(*store_, *identity_, params);
  ASSERT_TRUE(static_cast<bool>(offered)) << offered.error().message;
  EXPECT_EQ(offered->state, PaymentPromiseState::Offered);
  EXPECT_EQ(offered->payer_account_id, local_account_id_);
  EXPECT_EQ(offered->payee_account_id, "account:bob-counterparty");
  EXPECT_FALSE(offered->payer_signature_b64.empty());
  EXPECT_TRUE(offered->payee_signature_b64.empty());

  auto pk = Base64Decode(account_public_key_b64_);
  ASSERT_TRUE(static_cast<bool>(pk)) << pk.error().message;
  auto ok = PaymentPromiseCodec::VerifyPromiseSignature(*pk, *offered, offered->payer_signature_b64);
  ASSERT_TRUE(static_cast<bool>(ok)) << ok.error().message;
  EXPECT_TRUE(*ok);

  auto accepted = PaymentPromiseLifecycle::Accept(*store_, *identity_, offered->promise_id);
  ASSERT_TRUE(static_cast<bool>(accepted)) << accepted.error().message;
  EXPECT_EQ(accepted->state, PaymentPromiseState::Accepted);

  auto delivering = PaymentPromiseLifecycle::MarkDelivering(*store_, offered->promise_id);
  ASSERT_TRUE(static_cast<bool>(delivering)) << delivering.error().message;
  EXPECT_EQ(delivering->state, PaymentPromiseState::Delivering);

  auto released = PaymentPromiseLifecycle::RecordOutcome(*store_, *identity_, offered->promise_id,
                                                         PaymentPromiseState::Released, "delivered");
  ASSERT_TRUE(static_cast<bool>(released)) << released.error().message;
  EXPECT_EQ(released->state, PaymentPromiseState::Released);
  EXPECT_EQ(released->outcome_actor_account_id, local_account_id_);
  EXPECT_FALSE(released->outcome_signature_b64.empty());

  auto outcome_ok = PaymentPromiseCodec::VerifyOutcomeSignature(*pk, *released);
  ASSERT_TRUE(static_cast<bool>(outcome_ok)) << outcome_ok.error().message;
  EXPECT_TRUE(*outcome_ok);

  auto listed = store_->ListForAccount(local_account_id_);
  ASSERT_TRUE(static_cast<bool>(listed));
  ASSERT_EQ(listed->size(), 1u);
}

TEST_F(PaymentPromiseLifecycleTest, AvoidViaLifecycle) {
  PaymentPromiseLifecycle::OfferParams params;
  params.counterparty_account_id = "account:bob-counterparty";
  params.local_is_payer = true;
  params.amount_minor = 1;
  auto offered = PaymentPromiseLifecycle::CreateOffer(*store_, *identity_, params);
  ASSERT_TRUE(static_cast<bool>(offered)) << offered.error().message;

  ContactsStore contacts(data_dir_.string());
  Contact contact;
  contact.id = "contact-bob";
  contact.local.display_name = "Bob";
  contact.remote.ids = {{ContactIdKind::Account, "account:bob-counterparty", true}};
  contact.local.trust = TrustLevel::Unknown;
  SyncContactMirrors(contact);
  ASSERT_TRUE(static_cast<bool>(contacts.Upsert(contact)));

  auto avoided =
      PaymentPromiseLifecycle::AvoidCounterparty(*store_, contacts, *identity_, offered->promise_id);
  ASSERT_TRUE(static_cast<bool>(avoided)) << avoided.error().message;

  auto got = store_->Get(offered->promise_id);
  ASSERT_TRUE(static_cast<bool>(got));
  ASSERT_TRUE(got->has_value());
  EXPECT_TRUE((*got)->local_avoid);

  auto hit = contacts.FindByIdentity("account:bob-counterparty", ContactIdKind::Account);
  ASSERT_TRUE(static_cast<bool>(hit));
  ASSERT_TRUE(hit->has_value());
  EXPECT_EQ((*hit)->local.trust, TrustLevel::Blocked);
}

} // namespace
