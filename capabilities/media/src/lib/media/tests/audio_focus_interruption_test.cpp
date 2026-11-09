#include "lib/media/AudioFocusInterruption.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

using Change = AudioFocusInterruption::Change;
using Step = AudioFocusInterruption::Step;

// AudioManager's AUDIOFOCUS_* values reach the policy as what they mean.
TEST(AudioFocusInterruptionTest, AndroidsFocusChangesReadAsTheirMeaning) {
  EXPECT_EQ(AudioFocusInterruption::FromAndroid(1), Change::Gain);
  EXPECT_EQ(AudioFocusInterruption::FromAndroid(2), Change::Gain) << "a transient gain is the focus back too";
  EXPECT_EQ(AudioFocusInterruption::FromAndroid(-1), Change::Loss);
  EXPECT_EQ(AudioFocusInterruption::FromAndroid(-2), Change::LossTransient);
  EXPECT_EQ(AudioFocusInterruption::FromAndroid(-3), Change::LossTransientCanDuck);
  EXPECT_EQ(AudioFocusInterruption::FromAndroid(0), Change::Unknown);
}

// V060: a phone call (a transient loss) puts the live call on hold until the focus is back; the end puts the call's
// audio route back before the devices reopen.
TEST(AudioFocusInterruptionTest, ATransientLossHoldsTheCallUntilTheFocusIsBack) {
  AudioFocusInterruption focus;
  EXPECT_EQ(focus.OnChange(Change::LossTransient, /*session_active=*/true, /*interrupted=*/false), Step::Begin);
  EXPECT_FALSE(focus.WantsRetake(true)) << "Android gives a transient loss back by itself";
  EXPECT_EQ(focus.OnChange(Change::Gain, true, /*interrupted=*/true), Step::RestoreAndEnd);
}

// A duck is not a loss; a loss with no live session holds nothing; a gain without an interruption ends nothing.
TEST(AudioFocusInterruptionTest, OnlyALossDuringALiveSessionHoldsIt) {
  AudioFocusInterruption focus;
  EXPECT_EQ(focus.OnChange(Change::LossTransientCanDuck, true, false), Step::None);
  EXPECT_EQ(focus.OnChange(Change::Loss, /*session_active=*/false, false), Step::None);
  EXPECT_FALSE(focus.WantsRetake(true)) << "lost outside a live session: nothing to ask for";
  EXPECT_EQ(focus.OnChange(Change::Gain, true, /*interrupted=*/false), Step::None);
}

// Another app took the focus for good (its music or video): Android never gives it back, so the call stayed on hold
// and muted until it ended. The app asks again when the user is back; granted, the interruption ends.
TEST(AudioFocusInterruptionTest, APermanentLossEndsWhenAskingAgainIsGranted) {
  AudioFocusInterruption focus;
  EXPECT_EQ(focus.OnChange(Change::Loss, true, false), Step::Begin);
  EXPECT_TRUE(focus.WantsRetake(true));
  EXPECT_FALSE(focus.WantsRetake(/*session_active=*/false)) << "the call is over: nothing to ask for";

  EXPECT_EQ(focus.OnRetake(/*granted=*/false, /*interrupted=*/true), Step::None) << "still taken: still on hold";
  EXPECT_TRUE(focus.WantsRetake(true)) << "asked again next time";

  EXPECT_EQ(focus.OnRetake(/*granted=*/true, /*interrupted=*/true), Step::RestoreAndEnd);
  EXPECT_FALSE(focus.WantsRetake(true));
}

// Asked again while the other call still has the audio (Android delays the grant): the focus coming back later ends it.
TEST(AudioFocusInterruptionTest, ADelayedGrantEndsItWhenTheFocusComes) {
  AudioFocusInterruption focus;
  focus.OnChange(Change::Loss, true, false);
  EXPECT_EQ(focus.OnRetake(false, true), Step::None);
  EXPECT_EQ(focus.OnChange(Change::Gain, true, true), Step::RestoreAndEnd);
  EXPECT_FALSE(focus.WantsRetake(true));
}

// A transient loss that turns permanent (a phone call, then music) is one interruption, which asking again ends.
TEST(AudioFocusInterruptionTest, ATransientLossTurningPermanentIsOneInterruption) {
  AudioFocusInterruption focus;
  EXPECT_EQ(focus.OnChange(Change::LossTransient, true, false), Step::Begin);
  EXPECT_EQ(focus.OnChange(Change::Loss, true, /*interrupted=*/true), Step::None) << "already on hold";
  EXPECT_TRUE(focus.WantsRetake(true));
  EXPECT_EQ(focus.OnRetake(true, true), Step::RestoreAndEnd);
}

// The live session's end forgets a permanent loss: the next call asks for the focus afresh.
TEST(AudioFocusInterruptionTest, TheSessionsEndForgetsTheLoss) {
  AudioFocusInterruption focus;
  focus.OnChange(Change::Loss, true, false);
  focus.Reset();
  EXPECT_FALSE(focus.WantsRetake(true));
}

} // namespace
} // namespace pbr
