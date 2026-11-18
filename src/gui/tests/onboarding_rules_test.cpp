#include "gui/onboarding/OnboardingRules.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace pbr;

// The wizard shows once, after the identity is ready, and starts where the account stands.
TEST(OnboardingRulesTest, StartsAtNameForAFreshAccountAndAtAccountWhenRegistered) {
  EXPECT_EQ(OnboardingStartStep({.identity_ready = true}), OnboardingStep::Name);
  EXPECT_EQ(OnboardingStartStep({.identity_ready = true, .registered = true}), OnboardingStep::Account);
}

TEST(OnboardingRulesTest, NeverBeforeIdentityOrAfterCompletion) {
  EXPECT_EQ(OnboardingStartStep({.identity_ready = false}), OnboardingStep::None);
  EXPECT_EQ(OnboardingStartStep({.identity_ready = true, .completed = true}), OnboardingStep::None);
  EXPECT_EQ(OnboardingStartStep({.identity_ready = true, .registered = true, .completed = true}), OnboardingStep::None);
}

TEST(OnboardingRulesTest, StepsRunNameAccountFriendsThenEnd) {
  EXPECT_EQ(OnboardingNextStep(OnboardingStep::Name), OnboardingStep::Account);
  EXPECT_EQ(OnboardingNextStep(OnboardingStep::Account), OnboardingStep::Friends);
  EXPECT_EQ(OnboardingNextStep(OnboardingStep::Friends), OnboardingStep::None);
  EXPECT_STREQ(OnboardingStepKey(OnboardingStep::Name), "name");
  EXPECT_STREQ(OnboardingStepKey(OnboardingStep::None), "");
}

// The fresh identity's placeholder nickname is not offered as a typed name: the field starts empty (grey hint).
TEST(OnboardingRulesTest, TheNameFieldStartsEmptyForThePlaceholderNickname) {
  EXPECT_EQ(OnboardingNameFieldStart("user"), "");
  EXPECT_EQ(OnboardingNameFieldStart("Ann"), "Ann");
  EXPECT_EQ(OnboardingNameFieldStart(""), "");
}

// The continue button of the name step stays off while the field is empty.
TEST(OnboardingRulesTest, TheRegisterButtonIsOffWhileTheNameIsEmpty) {
  std::ifstream in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / "views/onboarding.rml");
  std::stringstream rml;
  rml << in.rdbuf();
  EXPECT_TRUE(OnboardingNameIsBlank(""));
  EXPECT_TRUE(OnboardingNameIsBlank("  \t\n"));
  EXPECT_FALSE(OnboardingNameIsBlank(" Ann "));
  EXPECT_NE(rml.str().find("data-attrif-disabled=\"busy || name_blank\""), std::string::npos);
}
