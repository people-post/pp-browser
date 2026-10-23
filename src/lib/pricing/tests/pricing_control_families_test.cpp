#include "lib/pricing/PricingControlFamilies.h"

#include "lib/pricing/InitiationBillingCodec.h"
#include "lib/pricing/InitiationBillingStore.h"
#include "lib/pricing/PaymentPromiseCodec.h"
#include "lib/pricing/PaymentPromiseStore.h"
#include "foundation/crypto/MlDsa.h"
#include "common/ValueJson.h"
#include "lib/pricing/PaymentPromiseWireCodec.h"
#include "common/Utilities.h"

#include <filesystem>
#include <gtest/gtest.h>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

constexpr const char* kAlice = "account:alice";
constexpr const char* kBob = "account:bob";

SystemControlContext From(const std::string& sender) {
  SystemControlContext context;
  context.sender_identity = sender;
  return context;
}

class PricingControlFamiliesTest : public ::testing::Test {
protected:
  void SetUp() override {
    data_dir_ = std::filesystem::temp_directory_path() / ("pp_pricing_families_" + util::GenerateUuid());
    std::filesystem::create_directories(data_dir_);
    billing_ = std::make_unique<InitiationBillingStore>(data_dir_.string());
    promises_ = std::make_unique<PaymentPromiseStore>(data_dir_.string());
  }

  void TearDown() override {
    billing_.reset();
    promises_.reset();
    std::filesystem::remove_all(data_dir_);
  }

  SystemControlFamily Billing(int64_t local_floor = 0) {
    return MakeInitiationBillingFamily(*billing_, [local_floor]() { return local_floor; });
  }

  static ThreadMessage ChargeRequired(const std::string& named, int64_t floor) {
    ChargeRequiredDetail detail;
    detail.peer_identity = named;
    detail.floor_minor = floor;
    auto json = InitiationBillingCodec::EncodeChargeRequired(detail);
    EXPECT_TRUE(json);
    auto message =
        InitiationBillingCodec::BuildSystemMessage("t-1", InitiationBillingControlType::ChargeRequired, *json, "x");
    EXPECT_TRUE(message);
    return *message;
  }

  /** Bob's signed offer, as a wire message. */
  ThreadMessage PromiseOffer(const std::string& payer = kAlice, const std::string& payee = kBob) const {
    PaymentPromise promise;
    promise.promise_id = "promise:" + util::GenerateUuid();
    promise.payer_account_id = payer;
    promise.payee_account_id = payee;
    promise.amount_minor = 100;
    promise.created_at_ms = 1700000000000;
    promise.payee_signature_b64 = *PaymentPromiseCodec::SignPromise(bob_.secret_key, promise);
    auto message =
        PaymentPromiseWireCodec::BuildSystemMessage("t-1", PaymentPromiseControlType::PromiseOffer, promise, "x");
    EXPECT_TRUE(message);
    return *message;
  }

  /** Our family: we are Alice; only Bob's key is known. */
  SystemControlFamily Promises() {
    return MakePaymentPromiseFamily(
        *promises_, []() -> std::optional<std::string> { return std::string(kAlice); },
        [this](const std::string& account) -> Roe<ByteVector> {
          if (account != kBob) {
            return Error("unknown account");
          }
          return bob_.public_key;
        });
  }

  size_t Pending() const {
    auto pending = promises_->ListPendingInbound();
    EXPECT_TRUE(pending);
    return pending ? pending->size() : 0;
  }

  MlDsaKeyPair bob_ = *MlDsa::GenerateKeyPair();
  std::filesystem::path data_dir_;
  std::unique_ptr<InitiationBillingStore> billing_;
  std::unique_ptr<PaymentPromiseStore> promises_;
};

TEST_F(PricingControlFamiliesTest, ChargeRequiredClosesTheSendersRelationship) {
  auto family = Billing();
  ASSERT_TRUE(billing_->MarkOpen(kAlice));
  auto message = ChargeRequired(kAlice, 250);
  ASSERT_TRUE(family.owns(message));
  ASSERT_TRUE(family.apply(message, From(kAlice)));
  EXPECT_FALSE(billing_->IsOpen(kAlice));
  EXPECT_EQ(billing_->Get(kAlice).floor_minor, 250);
}

TEST_F(PricingControlFamiliesTest, ChargeRequiredWithoutANameIsTheSenders) {
  auto family = Billing();
  auto message = ChargeRequired("", 90);
  ASSERT_TRUE(family.apply(message, From(kAlice)));
  EXPECT_EQ(billing_->Get(kAlice).floor_minor, 90);
}

TEST_F(PricingControlFamiliesTest, NoPeerMayCloseAnothersRelationship) {
  auto family = Billing();
  ASSERT_TRUE(billing_->MarkOpen(kBob));
  auto message = ChargeRequired(kBob, 999);
  EXPECT_FALSE(family.apply(message, From(kAlice)));
  EXPECT_TRUE(billing_->IsOpen(kBob));
  EXPECT_EQ(billing_->Get(kBob).floor_minor, 0);
  EXPECT_FALSE(family.apply(message, From("")));
}

TEST_F(PricingControlFamiliesTest, BillingStaysInTheChatStream) {
  auto family = Billing();
  EXPECT_FALSE(family.on_lane) << "billing is chat-stream history";
  EXPECT_FALSE(family.owns(PromiseOffer()));
}

TEST_F(PricingControlFamiliesTest, SignedOfferFromTheCounterpartyIsStaged) {
  auto family = Promises();
  auto message = PromiseOffer();
  ASSERT_TRUE(family.owns(message));
  ASSERT_TRUE(family.apply(message, From(kBob)));
  EXPECT_EQ(Pending(), 1u);
  auto committed = promises_->List();
  ASSERT_TRUE(committed);
  EXPECT_TRUE(committed->empty()) << "staged only: committing takes the user's accept";
}

TEST_F(PricingControlFamiliesTest, OfferFromAnyoneButTheCounterpartyIsRefused) {
  auto family = Promises();
  auto message = PromiseOffer();
  EXPECT_FALSE(family.apply(message, From("account:mallory")));
  EXPECT_FALSE(family.apply(message, From(kAlice)));
  EXPECT_FALSE(family.apply(message, From("")));
  auto not_ours = PromiseOffer("account:carol", kBob);
  EXPECT_FALSE(family.apply(not_ours, From(kBob))) << "not ours";
  EXPECT_EQ(Pending(), 0u);
}

TEST_F(PricingControlFamiliesTest, OfferWithAForgedSignatureIsRefused) {
  auto family = Promises();
  auto message = PromiseOffer();
  auto payload = TryParseObject(message.payload_json);
  ASSERT_TRUE(payload);
  auto promise = PaymentPromiseWireCodec::DecodeDetail(*payload->getString("detail"));
  ASSERT_TRUE(promise);
  promise->amount_minor = 1'000'000;
  auto forged = PaymentPromiseWireCodec::BuildSystemMessage("t-1", PaymentPromiseControlType::PromiseOffer, *promise, "x");
  ASSERT_TRUE(forged);
  EXPECT_FALSE(family.apply(*forged, From(kBob)));
  EXPECT_EQ(Pending(), 0u);
}

} // namespace
} // namespace pbr
