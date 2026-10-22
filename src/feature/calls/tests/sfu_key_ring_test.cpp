// V003: a group call's key rotates on every leave; a hop attach seals with the newest epoch and
// opens the previous one only for a short grace.
#include "feature/calls/SfuKeyRing.h"

#include <gtest/gtest.h>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

ByteVector KeyOf(uint8_t fill) { return ByteVector(32, fill); }

TEST(SfuKeyRingTest, SealsWithTheNewestAndOpensThePreviousForAGrace) {
  SfuKeyRing ring;
  EXPECT_TRUE(ring.Current().key.empty());
  EXPECT_TRUE(ring.ForOpen(0).empty());

  ring.Install(1, KeyOf(1), 1000);
  EXPECT_EQ(ring.Current().epoch, 1u);
  ASSERT_EQ(ring.ForOpen(1000).size(), 1u);

  ring.Install(2, KeyOf(2), 2000);
  EXPECT_EQ(ring.Current().epoch, 2u) << "a rotation seals with the new key at once";
  EXPECT_EQ(ring.Current().key, KeyOf(2));
  auto open = ring.ForOpen(2000 + SfuKeyRing::kPreviousEpochGraceMs - 1);
  ASSERT_EQ(open.size(), 2u);
  EXPECT_EQ(open[0].epoch, 2u) << "the newest first";
  EXPECT_EQ(open[1].epoch, 1u) << "the previous while the others switch";
  EXPECT_EQ(ring.ForOpen(2000 + SfuKeyRing::kPreviousEpochGraceMs).size(), 1u)
      << "after the grace, the leaver's key opens nothing";
}

TEST(SfuKeyRingTest, OnlyMovesForward) {
  SfuKeyRing ring;
  ring.Install(3, KeyOf(3), 0);
  ring.Install(2, KeyOf(2), 10);  // a late CallMediaKey for an older epoch
  ring.Install(3, KeyOf(9), 20);  // the same epoch again
  EXPECT_EQ(ring.Current().epoch, 3u);
  EXPECT_EQ(ring.Current().key, KeyOf(3));
  EXPECT_EQ(ring.ForOpen(20).size(), 1u);
}

} // namespace
} // namespace pbr
