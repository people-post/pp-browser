#include "gui/CallHoldMute.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// Another call takes the audio: our open mic is muted, and opened again when the hold ends.
TEST(CallHoldMuteTest, AHoldMutesAnOpenMicAndTheEndOpensItAgain) {
  CallHoldMute mute;
  EXPECT_EQ(mute.Next(/*held=*/true, /*muted_now=*/false), std::optional<bool>(true));
  EXPECT_EQ(mute.Next(/*held=*/false, /*muted_now=*/true), std::optional<bool>(false));
}

// The user had muted before the hold: the hold leaves the mic alone, and its end does not unmute them.
TEST(CallHoldMuteTest, AUserWhoHadMutedStaysMuted) {
  CallHoldMute mute;
  EXPECT_EQ(mute.Next(true, /*muted_now=*/true), std::nullopt);
  EXPECT_EQ(mute.Next(false, true), std::nullopt);
}

// Each hold decides afresh: after a hold that muted, a later one over a user's own mute still leaves it.
TEST(CallHoldMuteTest, EachHoldDecidesAfresh) {
  CallHoldMute mute;
  EXPECT_EQ(mute.Next(true, false), std::optional<bool>(true));
  EXPECT_EQ(mute.Next(false, true), std::optional<bool>(false));
  EXPECT_EQ(mute.Next(true, true), std::nullopt);
  EXPECT_EQ(mute.Next(false, true), std::nullopt);
  EXPECT_EQ(mute.Next(false, false), std::nullopt) << "no hold, nothing to undo";
}

} // namespace
} // namespace pbr

TEST(CallHoldMuteTest, AUserWhoTouchedMuteDuringTheHoldKeepsTheirChoice) {
  CallHoldMute mute;
  EXPECT_EQ(mute.Next(true, false), std::optional<bool>(true));
  mute.UserToggled();  // they unmuted (or muted again) by hand
  EXPECT_EQ(mute.Next(false, true), std::nullopt) << "the end of the hold does not undo the user's choice";
  EXPECT_EQ(mute.Next(false, false), std::nullopt);
}

