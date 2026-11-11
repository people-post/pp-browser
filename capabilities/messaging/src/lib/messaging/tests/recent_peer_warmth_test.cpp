// The chat / call warm mark lasts kRecentPeerWarmMs after the last call or message with a peer, not forever (#577).
#include "lib/messaging/RecentPeerWarmth.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

using Keys = std::vector<std::string>;

TEST(RecentPeerWarmthTest, AWarmedPeerLapsesAfterTheSpellWithNoActivity) {
  RecentPeerWarmth warmth;
  EXPECT_TRUE(warmth.Warm("bob", 1000)) << "a call: bob was cold";
  EXPECT_TRUE(warmth.IsWarm("bob"));
  EXPECT_EQ(warmth.Lapsed(1000 + kRecentPeerWarmMs - 1), Keys{}) << "still inside the spell";
  EXPECT_EQ(warmth.Lapsed(1000 + kRecentPeerWarmMs), Keys{"bob"});
  EXPECT_FALSE(warmth.IsWarm("bob"));
  EXPECT_EQ(warmth.Lapsed(1000 + 2 * kRecentPeerWarmMs), Keys{}) << "lapses once";
}

TEST(RecentPeerWarmthTest, AMessageKeepsAWarmPeerWarm) {
  RecentPeerWarmth warmth(600);
  warmth.Warm("bob", 0);
  EXPECT_TRUE(warmth.Touch("bob", 500));
  EXPECT_EQ(warmth.Lapsed(800), Keys{}) << "600 ms from the message, not from the call";
  EXPECT_EQ(warmth.Lapsed(1100), Keys{"bob"});
}

TEST(RecentPeerWarmthTest, AnotherCallStartsTheSpellAgain) {
  RecentPeerWarmth warmth(600);
  warmth.Warm("bob", 0);
  EXPECT_FALSE(warmth.Warm("bob", 400)) << "already warm";
  EXPECT_EQ(warmth.Lapsed(900), Keys{});
  EXPECT_EQ(warmth.Lapsed(1000), Keys{"bob"});
}

TEST(RecentPeerWarmthTest, AMessageDoesNotWarmAColdPeer) {
  RecentPeerWarmth warmth(600);
  EXPECT_FALSE(warmth.Touch("carol", 0)) << "a message alone keeps no carrier";
  EXPECT_FALSE(warmth.IsWarm("carol"));
  warmth.Warm("bob", 0);
  warmth.Touch("bob", 300);
  EXPECT_EQ(warmth.Lapsed(600), Keys{}) << "only bob, still warm";
}

TEST(RecentPeerWarmthTest, EachPeerLapsesOnItsOwnClockAndAnOlderMomentNeverShortensASpell) {
  RecentPeerWarmth warmth(600);
  warmth.Warm("bob", 0);
  warmth.Warm("carol", 300);
  warmth.Touch("carol", 100); // a late report of an earlier message
  EXPECT_EQ(warmth.Lapsed(600), Keys{"bob"});
  EXPECT_EQ(warmth.Lapsed(899), Keys{});
  EXPECT_EQ(warmth.Lapsed(900), Keys{"carol"});
  EXPECT_FALSE(warmth.Warm("", 0)) << "no peer, nothing warmed";
}

} // namespace
} // namespace pbr
