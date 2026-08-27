#include "base/data/Config.h"
#include "base/data/ConfigJson.h"
#include "base/data/PricingTypes.h"
#include "common/ValueJson.h"

#include <gtest/gtest.h>

namespace {

TEST(PricingConfigTest, RoundTripsInitiationFloor) {
  pbr::AppConfig config;
  config.initiation_floor = 1234;
  const pbr::Object out = pbr::AppConfigToObject(config);
  ASSERT_TRUE(out.contains("initiation_floor"));
  EXPECT_EQ(out.getIf<int64_t>("initiation_floor"), 1234);

  pbr::AppConfig parsed;
  pbr::AppConfigFromObject(out, parsed);
  EXPECT_EQ(parsed.initiation_floor, 1234);

  pbr::AppConfig missing;
  pbr::Object bare;
  bare.set("theme", "themes/base.rcss");
  pbr::AppConfigFromObject(bare, missing);
  EXPECT_EQ(missing.initiation_floor, 0);
}

TEST(PricingTypesTest, CurrencyStub) {
  EXPECT_STREQ(pbr::kPricingCurrencyId, "pp_credit");
  EXPECT_STREQ(pbr::kPricingCurrencyDisplayName, "Credits");
  EXPECT_FALSE(pbr::PaymentRailsAvailable());
}

} // namespace
