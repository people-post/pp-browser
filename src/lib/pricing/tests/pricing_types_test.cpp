#include "lib/pricing/PricingTypes.h"

#include <gtest/gtest.h>
#include "common/PbrCompat.h"

namespace {

TEST(PricingTypesTest, CurrencyStub) {
  EXPECT_STREQ(pbr::kPricingCurrencyId, "pp_credit");
  EXPECT_STREQ(pbr::kPricingCurrencyDisplayName, "Credits");
  EXPECT_FALSE(pbr::PaymentRailsAvailable());
}

} // namespace
