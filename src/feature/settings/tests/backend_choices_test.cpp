// Which services the "Service" row offers: in a store build staging / dev need both an address in the build and the
// tester options; a tester build never offers Production (release-compatibility RC022).
#include "feature/settings/BackendChoices.h"
#include "foundation/platform/DeploymentProfile.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

std::vector<std::string> OfferedIn(const bool prod, const bool staging, const bool dev, const bool tester) {
  std::vector<std::string> ids;
  for (const char* id : BackendChoicesOffered(prod, staging, dev, tester)) {
    ids.push_back(id);
  }
  return ids;
}

// A store build: prod is available.
std::vector<std::string> Offered(const bool staging, const bool dev, const bool tester) {
  return OfferedIn(true, staging, dev, tester);
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

TEST(BackendChoicesTest, ATesterBuildNeverOffersProductionAndShowsItsEnvironmentsWithoutTheTesterOptions) {
  for (const bool tester_options : {false, true}) {
    EXPECT_EQ(OfferedIn(false, true, true, tester_options), (std::vector<std::string>{"staging", "dev", "custom"}));
    EXPECT_EQ(OfferedIn(false, true, false, tester_options), (std::vector<std::string>{"staging", "custom"}));
    EXPECT_EQ(OfferedIn(false, false, true, tester_options), (std::vector<std::string>{"dev", "custom"}));
    // Neither compiled: only a custom backend (FallbackBackendIn is Custom there).
    EXPECT_EQ(OfferedIn(false, false, false, tester_options), (std::vector<std::string>{"custom"}));
  }
}

// The Settings row as this build's state fills it (NetworkSettingsSection: BackendAvailable(Prod)).
TEST(BackendChoicesTest, ProductionIsListedExactlyWhereTheBuildCanRunIt) {
  const bool prod = BackendAvailable(BackendEnv::Prod);
  EXPECT_EQ(prod, !IsTesterBuild());
  for (const bool tester_options : {false, true}) {
    const std::vector<std::string> ids =
        OfferedIn(prod, BackendAvailable(BackendEnv::Staging), BackendAvailable(BackendEnv::Dev), tester_options);
    EXPECT_EQ(ids.front() == "prod", prod);
    EXPECT_EQ(ids.back(), "custom");
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
