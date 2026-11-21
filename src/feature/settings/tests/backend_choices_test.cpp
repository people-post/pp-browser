// Which services the "Service" row offers: staging / dev need both an address in the build and the tester options.
#include "feature/settings/BackendChoices.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

std::vector<std::string> Offered(const bool staging, const bool dev, const bool tester) {
  std::vector<std::string> ids;
  for (const char* id : BackendChoicesOffered(staging, dev, tester)) {
    ids.push_back(id);
  }
  return ids;
}

} // namespace

TEST(BackendChoicesTest, StagingAndDevNeedTheBuildAndTheTesterOptions) {
  EXPECT_EQ(Offered(true, true, true), (std::vector<std::string>{"prod", "staging", "dev", "custom"}));
  EXPECT_EQ(Offered(true, false, true), (std::vector<std::string>{"prod", "staging", "custom"}));
  EXPECT_EQ(Offered(false, true, true), (std::vector<std::string>{"prod", "dev", "custom"}));
  EXPECT_EQ(Offered(true, true, false), (std::vector<std::string>{"prod", "custom"}));
  EXPECT_EQ(Offered(false, false, true), (std::vector<std::string>{"prod", "custom"}));
}

TEST(BackendChoicesTest, ProductionAndCustomAreAlwaysThere) {
  for (const bool staging : {false, true}) {
    for (const bool dev : {false, true}) {
      for (const bool tester : {false, true}) {
        const std::vector<std::string> ids = Offered(staging, dev, tester);
        EXPECT_EQ(ids.front(), "prod");
        EXPECT_EQ(ids.back(), "custom");
      }
    }
  }
}

TEST(BackendChoicesTest, OnlyADifferentEnvironmentAsksToConfirm) {
  EXPECT_FALSE(BackendSwitchNeedsConfirm("prod", "prod"));
  EXPECT_FALSE(BackendSwitchNeedsConfirm("qa", "staging")) << "the legacy name is the same environment";
  EXPECT_FALSE(BackendSwitchNeedsConfirm("custom", "custom"));
  EXPECT_TRUE(BackendSwitchNeedsConfirm("prod", "staging"));
  EXPECT_TRUE(BackendSwitchNeedsConfirm("staging", "dev"));
  EXPECT_TRUE(BackendSwitchNeedsConfirm("dev", "custom"));
  EXPECT_TRUE(BackendSwitchNeedsConfirm("qa", "prod"));
}

} // namespace pbr
