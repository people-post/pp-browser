#include "base/data/Config.h"
#include "base/data/ConfigJson.h"
#include "base/data/PricingTypes.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace {

TEST(PricingConfigTest, RoundTripsInitiationFloor) {
  pbr::AppConfig config;
  config.initiation_floor = 1234;
  nlohmann::json out;
  pbr::to_json(out, config);
  ASSERT_TRUE(out.contains("initiation_floor"));
  EXPECT_EQ(out["initiation_floor"], 1234);

  pbr::AppConfig parsed;
  pbr::from_json(out, parsed);
  EXPECT_EQ(parsed.initiation_floor, 1234);

  pbr::AppConfig missing;
  nlohmann::json bare = {{"theme", "themes/base.rcss"}};
  pbr::from_json(bare, missing);
  EXPECT_EQ(missing.initiation_floor, 0);
}

TEST(PricingTypesTest, CurrencyStub) {
  EXPECT_STREQ(pbr::kPricingCurrencyId, "pp_credit");
  EXPECT_STREQ(pbr::kPricingCurrencyDisplayName, "Credits");
  EXPECT_FALSE(pbr::PaymentRailsAvailable());
}

} // namespace
