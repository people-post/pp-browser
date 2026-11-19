#include "lib/scan/QrScanPolicy.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {
using std::chrono::milliseconds;

QrScanPolicy::Time At(const QrScanPolicy::Time base, const int64_t ms) {
  return base + milliseconds(ms);
}

struct Fixture {
  QrScanPolicy::Time t0 = QrScanPolicy::Time(std::chrono::seconds(100));
  QrScanPolicy policy;
  Fixture() { policy.Begin(t0); }
};

TEST(QrScanPolicyTest, NoProbeInTheFirstFourSecondsThenOneToTwoX) {
  Fixture f;
  EXPECT_FALSE(f.policy.ProbeTarget(At(f.t0, 0), 1.f));
  EXPECT_FALSE(f.policy.ProbeTarget(At(f.t0, 3999), 1.f));
  ASSERT_TRUE(f.policy.ProbeTarget(At(f.t0, 4000), 1.f));
  EXPECT_FLOAT_EQ(*f.policy.ProbeTarget(At(f.t0, 4000), 1.f), 2.f);
}

TEST(QrScanPolicyTest, ProbesAtMostEveryFourSecondsAndNeverReachThreeX) {
  Fixture f;
  f.policy.OnZoomChanged(At(f.t0, 4000));
  EXPECT_FALSE(f.policy.ProbeTarget(At(f.t0, 7999), 2.f));
  ASSERT_TRUE(f.policy.ProbeTarget(At(f.t0, 8000), 2.f));
  EXPECT_FLOAT_EQ(*f.policy.ProbeTarget(At(f.t0, 8000), 2.f), 1.f); // back to 1x, not on to 3x
  EXPECT_FLOAT_EQ(*f.policy.ProbeTarget(At(f.t0, 8000), 1.f), 2.f);
  EXPECT_EQ(f.policy.ZoomChanges(), 1);
}

TEST(QrScanPolicyTest, DecodesEveryHundredMillisecondsWithNothingInView) {
  Fixture f;
  EXPECT_TRUE(f.policy.DecodeDue(At(f.t0, 0)));
  f.policy.AfterDecode(At(f.t0, 0), false, 20);
  EXPECT_FALSE(f.policy.DecodeDue(At(f.t0, 99)));
  EXPECT_TRUE(f.policy.DecodeDue(At(f.t0, 100)));
}

TEST(QrScanPolicyTest, DecodesEveryFrameWhileACandidateWasSeenInTheLastSecond) {
  Fixture f;
  f.policy.AfterDecode(At(f.t0, 1000), true, 0);
  EXPECT_TRUE(f.policy.DecodeDue(At(f.t0, 1000)));
  // The candidate is gone from the next frames, but it was seen less than a second ago.
  f.policy.AfterDecode(At(f.t0, 1033), false, 0);
  EXPECT_TRUE(f.policy.DecodeDue(At(f.t0, 1033)));
  f.policy.AfterDecode(At(f.t0, 2001), false, 0);
  EXPECT_FALSE(f.policy.DecodeDue(At(f.t0, 2050)));
  EXPECT_TRUE(f.policy.DecodeDue(At(f.t0, 2101)));
}

TEST(QrScanPolicyTest, ASlowDecodeLeavesTheWorkerHalfItsTimeForThePreview) {
  Fixture f;
  f.policy.AfterDecode(At(f.t0, 1000), true, 60);
  EXPECT_FALSE(f.policy.DecodeDue(At(f.t0, 1059)));
  EXPECT_TRUE(f.policy.DecodeDue(At(f.t0, 1060)));
}

TEST(QrScanPolicyTest, ZoomDoesNotMoveWhileACandidateWasSeenInTheLastHalfSecond) {
  Fixture f;
  f.policy.AfterDecode(At(f.t0, 5000), true, 10);
  EXPECT_TRUE(f.policy.CandidateRecent(At(f.t0, 5499)));
  EXPECT_FALSE(f.policy.ProbeTarget(At(f.t0, 5499), 1.f));
  EXPECT_FALSE(f.policy.ShouldResetCandidateZoom(At(f.t0, 5499)));
  EXPECT_FALSE(f.policy.CandidateRecent(At(f.t0, 5500)));
}

TEST(QrScanPolicyTest, AZoomTowardACodeIsHeldFiveSecondsAfterItWasLastSeen) {
  Fixture f;
  f.policy.AfterDecode(At(f.t0, 5000), true, 10);
  EXPECT_FALSE(f.policy.ShouldResetCandidateZoom(At(f.t0, 6000)));
  EXPECT_FALSE(f.policy.ShouldResetCandidateZoom(At(f.t0, 10000)));
  EXPECT_TRUE(f.policy.ShouldResetCandidateZoom(At(f.t0, 10001)));
  // Seen again: the hold starts over.
  f.policy.AfterDecode(At(f.t0, 10001), true, 10);
  EXPECT_FALSE(f.policy.ShouldResetCandidateZoom(At(f.t0, 14000)));
}

TEST(QrScanPolicyTest, ElapsedAndZoomChangesFeedTheResultLine) {
  Fixture f;
  f.policy.OnZoomChanged(At(f.t0, 4000));
  f.policy.OnZoomChanged(At(f.t0, 9000));
  EXPECT_EQ(f.policy.ZoomChanges(), 2);
  EXPECT_EQ(f.policy.ElapsedMs(At(f.t0, 9500)), 9500);
}
} // namespace
} // namespace pbr
