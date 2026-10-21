#include "gui/onboarding/OnboardingRules.h"

#include <gtest/gtest.h>

using namespace pbr;

// The wizard shows once, after the identity is ready, and starts where the account stands.
TEST(OnboardingRulesTest, StartsAtNameForAFreshAccountAndAtAccountWhenRegistered) {
  EXPECT_EQ(OnboardingStartStep({.identity_ready = true}), OnboardingStep::Name);
  EXPECT_EQ(OnboardingStartStep({.identity_ready = true, .registered = true}), OnboardingStep::Account);
}

TEST(OnboardingRulesTest, NeverBeforeIdentityOrAfterCompletion) {
  EXPECT_EQ(OnboardingStartStep({.identity_ready = false}), OnboardingStep::None);
  EXPECT_EQ(OnboardingStartStep({.identity_ready = true, .completed = true}), OnboardingStep::None);
  EXPECT_EQ(OnboardingStartStep({.identity_ready = true, .registered = true, .completed = true}),
            OnboardingStep::None);
}

TEST(OnboardingRulesTest, StepsRunNameAccountFriendsThenEnd) {
  EXPECT_EQ(OnboardingNextStep(OnboardingStep::Name), OnboardingStep::Account);
  EXPECT_EQ(OnboardingNextStep(OnboardingStep::Account), OnboardingStep::Friends);
  EXPECT_EQ(OnboardingNextStep(OnboardingStep::Friends), OnboardingStep::None);
  EXPECT_STREQ(OnboardingStepKey(OnboardingStep::Name), "name");
  EXPECT_STREQ(OnboardingStepKey(OnboardingStep::None), "");
}
