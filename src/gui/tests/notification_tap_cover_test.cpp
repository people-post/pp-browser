#include "gui/shell/NotificationTapCover.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

using Clock = NotificationTapCover::Clock;
using Step = NotificationTapCover::Step;

TapCoverScreen Screen(const bool foreground, const bool held = false, const bool call = false) {
  TapCoverScreen screen;
  screen.foreground = foreground;
  screen.held_elsewhere = held;
  screen.call = call;
  return screen;
}

} // namespace

TEST(NotificationTapCoverTest, OnlyAMessageTapThatBroughtTheAppUpWithNoCallIsHeld) {
  TapCoverFacts facts;
  facts.message_tap = true;
  EXPECT_TRUE(NotificationTapCover::ShouldArm(facts)) << "background (or cold start) -> tap on a message";

  TapCoverFacts call_alert = facts;
  call_alert.message_tap = false;
  EXPECT_FALSE(NotificationTapCover::ShouldArm(call_alert)) << "a call alert's tap: its ring shows";

  TapCoverFacts shown = facts;
  shown.app_was_shown = true;
  EXPECT_FALSE(NotificationTapCover::ShouldArm(shown)) << "the screen was up (notification centre, desktop): as today";

  TapCoverFacts modal = facts;
  modal.os_modal_up = true;
  EXPECT_FALSE(NotificationTapCover::ShouldArm(modal)) << "a share sheet / Save dialog in flight: as today";

  TapCoverFacts in_call = facts;
  in_call.call_shown = true;
  EXPECT_FALSE(NotificationTapCover::ShouldArm(in_call)) << "a live call keeps its screen: as today";
}

TEST(NotificationTapCoverTest, TheCoverIsUpBeforeTheForegroundAndTheHoldCountsFromIt) {
  NotificationTapCover cover;
  const Clock::time_point t0{};
  EXPECT_EQ(cover.Evaluate(Screen(true), t0), Step::Released) << "not armed";

  cover.Arm();
  EXPECT_EQ(cover.Evaluate(Screen(false), t0), Step::Shown) << "in place before the first resumed frame";
  EXPECT_EQ(cover.Until(), std::nullopt) << "no clock before the foreground";
  // The phone's own unlock can sit between the tap and the foreground.
  const Clock::time_point fg = t0 + std::chrono::seconds(20);
  EXPECT_EQ(cover.Evaluate(Screen(false), fg), Step::Shown);
  EXPECT_EQ(cover.Evaluate(Screen(true), fg), Step::Shown);
  ASSERT_TRUE(cover.Until());
  EXPECT_EQ(*cover.Until(), fg + NotificationTapCover::kHold);
  EXPECT_EQ(cover.Evaluate(Screen(true), fg + NotificationTapCover::kHold - std::chrono::milliseconds(1)), Step::Shown);
  EXPECT_EQ(cover.Evaluate(Screen(true), fg + NotificationTapCover::kHold), Step::Expired) << "never blank for good";
  EXPECT_FALSE(cover.Armed());
  EXPECT_EQ(cover.Evaluate(Screen(true), fg + NotificationTapCover::kHold), Step::Released) << "expired once";
}

TEST(NotificationTapCoverTest, ThePinGateHoldsItAndTheClockStartsAgainAfterIt) {
  NotificationTapCover cover;
  const Clock::time_point t0{};
  cover.Arm();
  EXPECT_EQ(cover.Evaluate(Screen(true), t0), Step::Shown);
  EXPECT_EQ(cover.Evaluate(Screen(true, /*held=*/true), t0 + std::chrono::seconds(1)), Step::Hidden)
      << "the PIN gate (or the start's logo) is what shows";
  EXPECT_EQ(cover.Until(), std::nullopt);
  // A long PIN entry does not use up the hold.
  const Clock::time_point unlocked = t0 + std::chrono::seconds(60);
  EXPECT_EQ(cover.Evaluate(Screen(true), unlocked), Step::Shown);
  EXPECT_EQ(*cover.Until(), unlocked + NotificationTapCover::kHold);
}

TEST(NotificationTapCoverTest, ACallDropsItForGood) {
  NotificationTapCover cover;
  const Clock::time_point t0{};
  cover.Arm();
  EXPECT_EQ(cover.Evaluate(Screen(true, false, /*call=*/true), t0), Step::Released);
  EXPECT_FALSE(cover.Armed());
  EXPECT_EQ(cover.Evaluate(Screen(true), t0), Step::Released) << "not back once the ring ends";
}

TEST(NotificationTapCoverTest, ReleasedOnceAndANewTapStartsOver) {
  NotificationTapCover cover;
  const Clock::time_point t0{};
  EXPECT_FALSE(cover.Release()) << "nothing armed";
  cover.Arm();
  EXPECT_EQ(cover.Evaluate(Screen(true), t0), Step::Shown);
  EXPECT_TRUE(cover.Release());
  EXPECT_FALSE(cover.Release()) << "consumed once";

  cover.Arm();
  EXPECT_EQ(cover.Evaluate(Screen(true), t0), Step::Shown);
  cover.Arm(); // a second tap
  EXPECT_EQ(cover.Until(), std::nullopt);
  const Clock::time_point later = t0 + std::chrono::seconds(2);
  EXPECT_EQ(cover.Evaluate(Screen(true), later), Step::Shown);
  EXPECT_EQ(*cover.Until(), later + NotificationTapCover::kHold);
}

namespace {
TapOpenFacts Open(const bool call, const bool armed, const bool ready, const bool opened) {
  TapOpenFacts facts;
  facts.call_shown = call;
  facts.cover_armed = armed;
  facts.messaging_ready = ready;
  facts.opened = opened;
  return facts;
}
} // namespace

TEST(NotificationTapCoverTest, TheCoverGoesOnlyOnceTheTappedChatWasActuallySelected) {
  EXPECT_EQ(NotificationTapCover::EndOfTapOpen(Open(false, true, true, true)), TapOpenEnd::Release);
  EXPECT_EQ(NotificationTapCover::EndOfTapOpen(Open(false, true, true, false)), TapOpenEnd::ChatListThenRelease)
      << "ready but the chat is gone or would not open: the chat list, then the cover goes";
}

TEST(NotificationTapCoverTest, AChatThatCannotOpenYetKeepsTheCoverForTheLaterRoute) {
  EXPECT_TRUE(NotificationTapCover::DeferTapOpen(Open(false, true, false, false)))
      << "messaging not ready: nothing closed, nothing selected, the cover stays (bounded by its hold)";
  EXPECT_FALSE(NotificationTapCover::DeferTapOpen(Open(false, true, true, false)));
  EXPECT_FALSE(NotificationTapCover::DeferTapOpen(Open(false, false, false, false))) << "no cover: as today";
  EXPECT_FALSE(NotificationTapCover::DeferTapOpen(Open(true, true, false, false))) << "a call keeps its screen";
}

TEST(NotificationTapCoverTest, AMissingChatSwitchesToTheListOnlyForTheTapsOwnCover) {
  EXPECT_EQ(NotificationTapCover::EndOfTapOpen(Open(false, false, true, false)), TapOpenEnd::Nothing)
      << "a desktop banner for a deleted chat, or after the hold: do nothing, as before";
  EXPECT_EQ(NotificationTapCover::EndOfTapOpen(Open(false, false, true, true)), TapOpenEnd::Nothing);
  EXPECT_EQ(NotificationTapCover::EndOfTapOpen(Open(true, true, true, false)), TapOpenEnd::Release)
      << "a call: no list switch, the call keeps its screen";
}

} // namespace pbr
