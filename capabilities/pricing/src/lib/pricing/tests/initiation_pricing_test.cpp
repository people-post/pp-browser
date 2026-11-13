#include "lib/pricing/InitiationBillingCodec.h"
#include "lib/pricing/InitiationBillingStore.h"
#include "lib/pricing/InitiationPricing.h"
#include "lib/pricing/PricingTypes.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

namespace {

using namespace pbr;

TEST(InitiationPricingTest, FreeAmountAndRelayRate) {
  EXPECT_TRUE(IsFreeAmount(0));
  EXPECT_TRUE(IsFreeAmount(-1));
  EXPECT_FALSE(IsFreeAmount(1));
  EXPECT_TRUE(IsFreeRelayRate(0.0));
  EXPECT_FALSE(IsFreeRelayRate(0.01));
  EXPECT_STREQ(PricingModeLabel(0), "volunteer");
  EXPECT_STREQ(PricingModeLabel(5), "paid");
}

TEST(InitiationPricingTest, OfferFloorAndOutboundGate) {
  EXPECT_TRUE(OfferMeetsFloor(0, 0));
  EXPECT_TRUE(OfferMeetsFloor(10, 10));
  EXPECT_TRUE(OfferMeetsFloor(11, 10));
  EXPECT_FALSE(OfferMeetsFloor(9, 10));
  EXPECT_TRUE(static_cast<bool>(InitiationPricing::CheckOfferAgainstFloor(10, 10)));
  EXPECT_FALSE(static_cast<bool>(InitiationPricing::CheckOfferAgainstFloor(1, 10)));
  EXPECT_TRUE(static_cast<bool>(InitiationPricing::CheckOutboundPayable(0)));
  auto blocked = InitiationPricing::CheckOutboundPayable(5);
  ASSERT_FALSE(static_cast<bool>(blocked));
  EXPECT_NE(blocked.error().message.find("payment_unavailable"), std::string::npos);
  auto relay = InitiationPricing::CheckRelayQuotePayable(1.5);
  ASSERT_FALSE(static_cast<bool>(relay));
  EXPECT_NE(relay.error().message.find("media_relay"), std::string::npos);
  EXPECT_TRUE(static_cast<bool>(InitiationPricing::CheckRelayQuotePayable(0.0)));
}

TEST(InitiationBillingCodecTest, ChargeRequiredRoundTrip) {
  ChargeRequiredDetail detail;
  detail.peer_identity = "account:alice";
  detail.floor_minor = 42;
  detail.message = "please pay";
  auto encoded = InitiationBillingCodec::EncodeChargeRequired(detail);
  ASSERT_TRUE(static_cast<bool>(encoded));
  auto decoded = InitiationBillingCodec::DecodeChargeRequired(*encoded);
  ASSERT_TRUE(static_cast<bool>(decoded));
  EXPECT_EQ(decoded->peer_identity, "account:alice");
  EXPECT_EQ(decoded->floor_minor, 42);
  EXPECT_EQ(decoded->currency, kPricingCurrencyId);
  EXPECT_EQ(decoded->message, "please pay");
}

TEST(InitiationBillingCodecTest, AcceptDecisionWire) {
  EXPECT_STREQ(InitiationChargeDecisionToWire(InitiationChargeDecision::Waive), "waive");
  EXPECT_STREQ(InitiationChargeDecisionToWire(InitiationChargeDecision::TakeAll), "take_all");
  EXPECT_EQ(InitiationChargeDecisionFromWire("take_all"), InitiationChargeDecision::TakeAll);
  EXPECT_EQ(InitiationChargeDecisionFromWire("waive"), InitiationChargeDecision::Waive);
  InitiationAcceptDetail detail;
  detail.peer_identity = "account:bob";
  detail.offer_minor = 9;
  detail.decision = InitiationChargeDecision::TakeAll;
  auto encoded = InitiationBillingCodec::EncodeInitiationAccept(detail);
  ASSERT_TRUE(static_cast<bool>(encoded));
  auto decoded = InitiationBillingCodec::DecodeInitiationAccept(*encoded);
  ASSERT_TRUE(static_cast<bool>(decoded));
  EXPECT_EQ(decoded->decision, InitiationChargeDecision::TakeAll);
  EXPECT_EQ(decoded->offer_minor, 9);
}

TEST(InitiationBillingStoreTest, MarkClosedRelocks) {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("pp_initiation_closed_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  InitiationBillingStore store(dir.string());
  ASSERT_TRUE(store.Load());
  ASSERT_TRUE(store.MarkOpen("account:carol"));
  EXPECT_TRUE(store.IsOpen("account:carol"));
  ASSERT_TRUE(store.MarkClosed("account:carol"));
  EXPECT_FALSE(store.IsOpen("account:carol"));
  std::filesystem::remove_all(dir);
}

TEST(InitiationBillingStoreTest, PersistsState) {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("pp_initiation_billing_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  {
    InitiationBillingStore store(dir.string());
    ASSERT_TRUE(store.Load());
    ASSERT_TRUE(store.MarkOffered("account:bob", 7, 5));
    EXPECT_FALSE(store.IsOpen("account:bob"));
    ASSERT_TRUE(store.MarkOpen("account:bob"));
    EXPECT_TRUE(store.IsOpen("account:bob"));
  }
  {
    InitiationBillingStore store(dir.string());
    ASSERT_TRUE(store.Load());
    EXPECT_TRUE(store.IsOpen("account:bob"));
    EXPECT_EQ(store.Get("account:bob").offer_minor, 7);
    ASSERT_TRUE(store.MarkClosed("account:bob"));
    EXPECT_FALSE(store.IsOpen("account:bob"));
  }
  std::filesystem::remove_all(dir);
}

// initiation_billing.json 1 → 2 (multi-relay MR001): rows keyed by a retired `relay:` id are dropped; the rest stay.
TEST(InitiationBillingStoreTest, AVersion1FileDropsItsRelayKeyedRows) {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("pp_initiation_billing_v1_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  {
    std::ofstream out(dir / "initiation_billing.json");
    out << R"({"schema_version":1,"peers":{"relay:old":{"state":"open","floor_minor":3},)"
           R"("account:ann":{"state":"open","floor_minor":4}}})";
  }
  InitiationBillingStore store(dir.string());
  ASSERT_TRUE(store.Load());
  EXPECT_FALSE(store.IsOpen("relay:old"));
  EXPECT_TRUE(store.IsOpen("account:ann"));
  EXPECT_EQ(store.Get("account:ann").floor_minor, 4);
  std::filesystem::remove_all(dir);
}

} // namespace
