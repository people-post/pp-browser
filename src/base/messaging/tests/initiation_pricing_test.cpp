#include "base/messaging/InitiationBillingCodec.h"
#include "base/messaging/InitiationBillingStore.h"
#include "base/messaging/InitiationPricing.h"
#include "base/data/PricingTypes.h"

#include <filesystem>
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
  detail.peer_identity = "relay:alice";
  detail.floor_minor = 42;
  detail.message = "please pay";
  auto encoded = InitiationBillingCodec::EncodeChargeRequired(detail);
  ASSERT_TRUE(static_cast<bool>(encoded));
  auto decoded = InitiationBillingCodec::DecodeChargeRequired(*encoded);
  ASSERT_TRUE(static_cast<bool>(decoded));
  EXPECT_EQ(decoded->peer_identity, "relay:alice");
  EXPECT_EQ(decoded->floor_minor, 42);
  EXPECT_EQ(decoded->currency, kPricingCurrencyId);
  EXPECT_EQ(decoded->message, "please pay");
}

TEST(InitiationBillingStoreTest, PersistsState) {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("pp_initiation_billing_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  {
    InitiationBillingStore store(dir.string());
    ASSERT_TRUE(store.Load());
    ASSERT_TRUE(store.MarkOffered("relay:bob", 7, 5));
    EXPECT_FALSE(store.IsOpen("relay:bob"));
    ASSERT_TRUE(store.MarkOpen("relay:bob"));
    EXPECT_TRUE(store.IsOpen("relay:bob"));
  }
  {
    InitiationBillingStore store(dir.string());
    ASSERT_TRUE(store.Load());
    EXPECT_TRUE(store.IsOpen("relay:bob"));
    EXPECT_EQ(store.Get("relay:bob").offer_minor, 7);
    ASSERT_TRUE(store.MarkClosed("relay:bob"));
    EXPECT_FALSE(store.IsOpen("relay:bob"));
  }
  std::filesystem::remove_all(dir);
}

} // namespace
