#include "feature/presenters/CallRingSteps.h"

#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

using State = CallRingStep::State;

TEST(CallRingStepsTest, AwaitingTheInviteTheCallArrivedTheNetworkIsBeingJoinedNobodyIsKnownYet) {
  const auto steps = CallRingSteps(true, "");
  EXPECT_EQ(steps[0].key, "call.ring.step.received");
  EXPECT_EQ(steps[0].state, State::Done);
  EXPECT_EQ(steps[1].key, "call.ring.step.joining");
  EXPECT_EQ(steps[1].state, State::Current);
  EXPECT_EQ(steps[2].key, "call.ring.step.verifying");
  EXPECT_EQ(steps[2].state, State::Pending);
}

TEST(CallRingStepsTest, ARingingCallHasAllThreeDoneAndTheThirdIsTheCaller) {
  const auto steps = CallRingSteps(false, "Wang Xiaoming");
  EXPECT_EQ(steps[1].key, "call.ring.step.joined");
  for (const CallRingStep& step : steps) {
    EXPECT_EQ(step.state, State::Done);
  }
  EXPECT_TRUE(steps[2].key.empty());
  EXPECT_EQ(steps[2].text, "Wang Xiaoming");
}

TEST(CallRingStepsTest, TheFirstLineSaysVoiceOrVideoOnceTheCallIsKnownAndNeverWhileAwaiting) {
  EXPECT_EQ(CallRingSteps(false, "Bob", CallRingMedia::Voice)[0].key, "call.ring.step.received_voice");
  EXPECT_EQ(CallRingSteps(false, "Bob", CallRingMedia::Video)[0].key, "call.ring.step.received_video");
  EXPECT_EQ(CallRingSteps(false, "Bob", CallRingMedia::Unknown)[0].key, "call.ring.step.received");
  EXPECT_EQ(CallRingSteps(true, "", CallRingMedia::Video)[0].key, "call.ring.step.received") << "nothing is known yet";
  EXPECT_EQ(CallRingSteps(false, "Bob", CallRingMedia::Video)[0].state, State::Done);
}

TEST(CallRingStepsTest, AStrangerIsShownAsGivenAndTheAwaitingNameIsIgnored) {
  EXPECT_EQ(CallRingSteps(false, "Stranger")[2].text, "Stranger");
  EXPECT_EQ(CallRingSteps(true, "Stranger")[2].key, "call.ring.step.verifying") << "nobody is known yet";
  EXPECT_TRUE(CallRingSteps(true, "Stranger")[2].text.empty());
}

TEST(CallRingStepsTest, AGroupCallsThirdLineIsTheTitleItWasGiven) {
  const auto steps = CallRingSteps(false, "Weekend hike");
  EXPECT_EQ(steps[2].text, "Weekend hike");
  EXPECT_EQ(steps[2].state, State::Done);
}

TEST(CallRingStepsTest, EveryWordExistsInEnglishAndChinese) {
  ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  for (const CallRingMedia media : {CallRingMedia::Unknown, CallRingMedia::Voice, CallRingMedia::Video}) {
    for (const bool awaiting : {true, false}) {
      for (const CallRingStep& step : CallRingSteps(awaiting, "x", media)) {
        if (step.key.empty()) {
          continue;
        }
        for (const char* lang : {"en", "zh-Hans"}) {
          LocalizationService::Instance().SetPreferredLanguage(lang);
          EXPECT_NE(Tr(step.key), step.key) << step.key << " in " << lang;
        }
      }
    }
  }
}

} // namespace
} // namespace pbr
