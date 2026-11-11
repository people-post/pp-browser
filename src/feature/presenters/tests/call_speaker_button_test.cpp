#include "feature/presenters/CallSpeakerButton.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

using Change = IMediaView::AudioRouteChange;

// No tap handed to the engine: the button shows the route the OS has.
TEST(CallSpeakerButtonTest, WithoutATapItShowsTheRoute) {
  EXPECT_TRUE(CallSpeakerButtonOn(std::nullopt, Change::None, 0, true));
  EXPECT_FALSE(CallSpeakerButtonOn(std::nullopt, Change::Running, 0, false));
}

// A tap whose route change waits shows the tapped value for a while, then the real route (it may not be applied yet).
TEST(CallSpeakerButtonTest, AWaitingTapIsShownForALimitedTime) {
  const CallSpeakerTap tap{.on = true, .at_ms = 1'000};
  EXPECT_TRUE(CallSpeakerButtonOn(tap, Change::Queued, 1'000 + kSpeakerTapShownMs - 1, false));
  EXPECT_FALSE(CallSpeakerButtonOn(tap, Change::Queued, 1'000 + kSpeakerTapShownMs, false));
}

// Once the engine runs the change the tapped value holds, however long it takes, until it is applied or dropped.
TEST(CallSpeakerButtonTest, ARunningChangeHoldsTheTappedValue) {
  const CallSpeakerTap tap{.on = true, .at_ms = 1'000};
  EXPECT_TRUE(CallSpeakerButtonOn(tap, Change::Running, 60'000, false));
  EXPECT_FALSE(CallSpeakerButtonOn(tap, Change::None, 60'000, false)) << "dropped by the stop path: the real route";
  EXPECT_TRUE(CallSpeakerButtonOn(tap, Change::None, 60'000, true)) << "applied: the route is the tapped one";
}

} // namespace
} // namespace pbr
