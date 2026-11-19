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
  f.policy.AfterDecode(At(f.t0, 0), At(f.t0, 20), false);
  EXPECT_FALSE(f.policy.DecodeDue(At(f.t0, 119))); // 100 ms after the decode FINISHED
  EXPECT_TRUE(f.policy.DecodeDue(At(f.t0, 120)));
}

TEST(QrScanPolicyTest, DecodesSoonWhileACandidateWasSeenInTheLastSecond) {
  Fixture f;
  f.policy.AfterDecode(At(f.t0, 1000), At(f.t0, 1000), true);
  EXPECT_FALSE(f.policy.DecodeDue(At(f.t0, 1009)));
  EXPECT_TRUE(f.policy.DecodeDue(At(f.t0, 1010)));
  // The candidate is gone from the next frames, but it was seen less than a second ago.
  f.policy.AfterDecode(At(f.t0, 1033), At(f.t0, 1033), false);
  EXPECT_TRUE(f.policy.DecodeDue(At(f.t0, 1043)));
  f.policy.AfterDecode(At(f.t0, 2001), At(f.t0, 2001), false);
  EXPECT_FALSE(f.policy.DecodeDue(At(f.t0, 2050)));
  EXPECT_TRUE(f.policy.DecodeDue(At(f.t0, 2101)));
}

TEST(QrScanPolicyTest, ASlowDecodeLeavesTheWorkerHalfItsTimeForThePreview) {
  Fixture f;
  f.policy.AfterDecode(At(f.t0, 1000), At(f.t0, 1060), true);
  EXPECT_FALSE(f.policy.DecodeDue(At(f.t0, 1119))); // rests as long as the decode took, from when it finished
  EXPECT_TRUE(f.policy.DecodeDue(At(f.t0, 1120)));
}

TEST(QrScanPolicyTest, ACandidateInViewKeepsDecodingToHalfTheWorkersTimeAtMost) {
  // The real call pattern: frames arrive every 33 ms, a decode of 40 ms starts when a frame is due, and the policy is
  // told when it started and when it ended.
  Fixture f;
  int64_t busy_ms = 0;
  int decodes = 0;
  for (int64_t t = 0; t < 1000;) {
    if (f.policy.DecodeDue(At(f.t0, t))) {
      f.policy.AfterDecode(At(f.t0, t), At(f.t0, t + 40), true);
      busy_ms += 40;
      ++decodes;
      t += 40;
    }
    t = (t / 33 + 1) * 33; // next camera frame
  }
  EXPECT_LE(busy_ms, 520);
  EXPECT_GE(decodes, 8); // still several decode attempts a second
}

TEST(QrScanPolicyTest, ZoomDoesNotMoveWhileACandidateWasSeenInTheLastHalfSecond) {
  Fixture f;
  f.policy.AfterDecode(At(f.t0, 5000), At(f.t0, 5010), true);
  EXPECT_TRUE(f.policy.CandidateRecent(At(f.t0, 5499)));
  EXPECT_FALSE(f.policy.ProbeTarget(At(f.t0, 5499), 1.f));
  EXPECT_FALSE(f.policy.ShouldResetCandidateZoom(At(f.t0, 5499)));
  EXPECT_FALSE(f.policy.CandidateRecent(At(f.t0, 5500)));
}

TEST(QrScanPolicyTest, AZoomTowardACodeIsHeldFiveSecondsAfterItWasLastSeen) {
  Fixture f;
  f.policy.AfterDecode(At(f.t0, 5000), At(f.t0, 5010), true);
  EXPECT_FALSE(f.policy.ShouldResetCandidateZoom(At(f.t0, 6000)));
  EXPECT_FALSE(f.policy.ShouldResetCandidateZoom(At(f.t0, 10000)));
  EXPECT_TRUE(f.policy.ShouldResetCandidateZoom(At(f.t0, 10001)));
  // Seen again: the hold starts over.
  f.policy.AfterDecode(At(f.t0, 10001), At(f.t0, 10011), true);
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
