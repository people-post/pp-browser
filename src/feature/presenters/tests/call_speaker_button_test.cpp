#include "feature/presenters/CallSpeakerButton.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

using Change = IMediaView::AudioRouteChange;

// No tap handed to the engine: the button shows the route the OS has.
TEST(CallSpeakerButtonTest, WithoutATapItShowsTheRoute) {
  EXPECT_TRUE(CallSpeakerButtonOn(std::nullopt, Change::None, false, 0, true));
  EXPECT_FALSE(CallSpeakerButtonOn(std::nullopt, Change::Running, false, 0, false));
}

// A tap whose route change waits shows the tapped value for a while, then the real route (it may not be applied yet).
TEST(CallSpeakerButtonTest, AWaitingTapIsShownForALimitedTime) {
  const CallSpeakerTap tap{.on = true, .at_ms = 1'000};
  EXPECT_TRUE(CallSpeakerButtonOn(tap, Change::Queued, false, 1'000 + kSpeakerTapShownMs - 1, false));
  EXPECT_FALSE(CallSpeakerButtonOn(tap, Change::Queued, false, 1'000 + kSpeakerTapShownMs, false));
}

// Once the engine runs the change the tapped value holds, however long it takes, until it is applied or dropped.
TEST(CallSpeakerButtonTest, ARunningChangeHoldsTheTappedValue) {
  const CallSpeakerTap tap{.on = true, .at_ms = 1'000};
  EXPECT_TRUE(CallSpeakerButtonOn(tap, Change::Running, false, 60'000, false));
  EXPECT_FALSE(CallSpeakerButtonOn(tap, Change::None, false, 60'000, false))
      << "dropped by the stop path: the real route";
  EXPECT_TRUE(CallSpeakerButtonOn(tap, Change::None, false, 60'000, true)) << "applied: the route is the tapped one";
}

// A tap waiting behind the engine's reopen or audio rebuild keeps the tapped value however long that takes: the
// button does not flicker back to the old route and then forward again.
TEST(CallSpeakerButtonTest, AWaitingTapHoldsWhileTheDevicesReopen) {
  const CallSpeakerTap tap{.on = true, .at_ms = 1'000};
  EXPECT_TRUE(CallSpeakerButtonOn(tap, Change::Queued, true, 1'000 + 10 * kSpeakerTapShownMs, false));
  EXPECT_FALSE(CallSpeakerButtonOn(tap, Change::Queued, false, 1'000 + 10 * kSpeakerTapShownMs, false));
}

// Android with media up: the engine changes the route around the mic. The developer trigger asks for the old order
// once (route now, reopen after). Elsewhere, or with no media, the route just changes.
TEST(CallSpeakerButtonTest, ATapIsPlannedAroundTheMicOnlyWhereTheMicWouldBeLost) {
  const CallSpeakerTapPlan around = PlanCallSpeakerTap(/*needs_reopen=*/true, /*dev_old_order=*/false);
  EXPECT_TRUE(around.around_the_mic);
  EXPECT_FALSE(around.reopen_after);
  const CallSpeakerTapPlan old_order = PlanCallSpeakerTap(true, true);
  EXPECT_FALSE(old_order.around_the_mic);
  EXPECT_TRUE(old_order.reopen_after);
  for (const bool dev : {false, true}) {
    const CallSpeakerTapPlan plain = PlanCallSpeakerTap(false, dev);
    EXPECT_FALSE(plain.around_the_mic);
    EXPECT_FALSE(plain.reopen_after);
  }
}

} // namespace
} // namespace pbr
