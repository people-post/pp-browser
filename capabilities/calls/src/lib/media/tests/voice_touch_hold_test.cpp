#include "lib/media/VoiceTouchHold.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

using V = VoiceTouchHold::Verdict;

TEST(VoiceTouchHold, FingerStaysDownMeansHolding) {
  VoiceTouchHold hold;
  hold.Press({7});
  EXPECT_TRUE(hold.FollowsFinger());
  for (int i = 0; i < 50; ++i) {
    EXPECT_EQ(hold.Tick({7}), V::Holding);
  }
}

TEST(VoiceTouchHold, NoFingerDownIsLost) {
  VoiceTouchHold hold;
  hold.Press({7});
  EXPECT_EQ(hold.Tick({7}), V::Holding);
  EXPECT_EQ(hold.Tick({}), V::Lost); // lifted without a mouse-up ever arriving
  EXPECT_FALSE(hold.StillDown({}));
}

TEST(VoiceTouchHold, ASecondFingerLiftingDoesNotEndTheHold) {
  VoiceTouchHold hold;
  hold.Press({7});
  EXPECT_EQ(hold.Tick({7, 9}), V::Holding); // second finger lands
  EXPECT_EQ(hold.Tick({7}), V::Holding);    // and lifts: the holding finger is still there
  EXPECT_TRUE(hold.StillDown({7}));
}

TEST(VoiceTouchHold, TheHoldingFingerLiftingWhileAnotherStaysIsLost) {
  VoiceTouchHold hold;
  hold.Press({7});
  hold.Tick({7, 9});
  EXPECT_EQ(hold.Tick({9}), V::Lost); // identity, not a count: finger 9 is not the one that pressed
  EXPECT_FALSE(hold.StillDown({9}));
}

TEST(VoiceTouchHold, TheNewestFingerIsTheOneThatPressed) {
  VoiceTouchHold hold;
  hold.Press({3, 8}); // a resting finger and the new one
  EXPECT_EQ(hold.Tick({3, 8}), V::Holding);
  EXPECT_EQ(hold.Tick({3}), V::Lost);
}

TEST(VoiceTouchHold, ASeenButNotYetListedFingerIsAdoptedWithinTheFirstTicks) {
  VoiceTouchHold hold;
  hold.Press({}); // the mouse event came before SDL listed the finger
  EXPECT_FALSE(hold.FollowsFinger());
  EXPECT_EQ(hold.Tick({}), V::Holding);
  EXPECT_EQ(hold.Tick({4}), V::Holding);
  EXPECT_TRUE(hold.FollowsFinger());
  EXPECT_EQ(hold.Tick({}), V::Lost);
}

TEST(VoiceTouchHold, AMousePressNeverFollowsAFingerThatLandsLater) {
  VoiceTouchHold hold;
  hold.Press({});
  for (int i = 0; i < 6; ++i) {
    EXPECT_EQ(hold.Tick({}), V::Holding);
  }
  EXPECT_EQ(hold.Tick({5}), V::Holding); // too late to be the one that pressed
  EXPECT_FALSE(hold.FollowsFinger());
  EXPECT_TRUE(hold.StillDown({})); // the mouse-up is what ends it
}

} // namespace
} // namespace pbr
