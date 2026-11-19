#include "lib/media/VoipCategoryGuard.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

using namespace std::chrono_literals;
using Clock = VoipCategoryGuard::Clock;

/** A fake clock: the unit started at t0. */
const Clock::time_point t0 = Clock::time_point{} + 100s;

TEST(VoipCategoryGuardTest, AForeignChangeSoonAfterTheUnitStartedReopensOnce) {
  VoipCategoryGuard guard;
  guard.OnVoiceProcessingStarted(t0);
  EXPECT_TRUE(guard.OnCategoryChange(t0 + 300ms, /*ours=*/false));
  EXPECT_FALSE(guard.OnCategoryChange(t0 + 500ms, /*ours=*/false)) << "one reopen per call";
}

TEST(VoipCategoryGuardTest, TheReopenedUnitDoesNotEarnASecondReopen) {
  VoipCategoryGuard guard;
  guard.OnVoiceProcessingStarted(t0);
  ASSERT_TRUE(guard.OnCategoryChange(t0 + 300ms, false));
  guard.OnVoiceProcessingStarted(t0 + 600ms); // the reopen's unit
  EXPECT_FALSE(guard.OnCategoryChange(t0 + 700ms, false));
}

TEST(VoipCategoryGuardTest, AForeignChangeAfterTheWindowAsksNothing) {
  VoipCategoryGuard guard;
  guard.OnVoiceProcessingStarted(t0);
  EXPECT_FALSE(guard.OnCategoryChange(t0 + 2s, false));
}

TEST(VoipCategoryGuardTest, OurOwnConfigurationAsksNothing) {
  VoipCategoryGuard guard;
  guard.OnVoiceProcessingStarted(t0);
  EXPECT_FALSE(guard.OnCategoryChange(t0 + 300ms, /*ours=*/true)) << "the session is as the call set it";
  EXPECT_TRUE(guard.OnCategoryChange(t0 + 400ms, false)) << "and it did not use up the reopen";
}

TEST(VoipCategoryGuardTest, NothingBeforeTheUnitStarted) {
  VoipCategoryGuard guard;
  EXPECT_FALSE(guard.OnCategoryChange(t0, false));
  guard.OnVoiceProcessingStarted(t0);
  EXPECT_FALSE(guard.OnCategoryChange(t0 - 10ms, false)) << "a change from before the start";
}

TEST(VoipCategoryGuardTest, TheNextCallHasItsOwnReopen) {
  VoipCategoryGuard guard;
  guard.OnVoiceProcessingStarted(t0);
  ASSERT_TRUE(guard.OnCategoryChange(t0 + 300ms, false));
  guard.Reset(); // the call's session ended
  EXPECT_FALSE(guard.OnCategoryChange(t0 + 400ms, false)) << "no unit started in the new call yet";
  guard.OnVoiceProcessingStarted(t0 + 10s);
  EXPECT_TRUE(guard.OnCategoryChange(t0 + 10s + 300ms, false));
}

} // namespace
} // namespace pbr
