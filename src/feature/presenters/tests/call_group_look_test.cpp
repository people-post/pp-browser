// A call that became a group stays one when people leave; the 1:1 name comes from the people joined now.
#include "feature/presenters/CallGroupLook.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

CallScreen::Member Member(const std::string& id, const bool local = false) {
  CallScreen::Member m;
  m.identity = id;
  m.is_local = local;
  return m;
}

TEST(CallGroupLookTest, ThreeJoinedThenOneLeavesStaysAGroup) {
  CallGroupLook look;
  EXPECT_FALSE(look.Observe("c1", false, 2)) << "a 1:1 call";
  EXPECT_TRUE(look.Observe("c1", false, 3)) << "grown to three";
  EXPECT_TRUE(look.Observe("c1", false, 2)) << "one left: still the group look (roster, silent tiles)";
  EXPECT_TRUE(look.Observe("c1", false, 1)) << "alone: still a group, so the countdown line shows";
}

TEST(CallGroupLookTest, AGroupIdIsAGroupFromTheStart) {
  CallGroupLook look;
  EXPECT_TRUE(look.Observe("c1", true, 2));
  EXPECT_TRUE(look.Observe("c1", true, 1));
}

TEST(CallGroupLookTest, ANewCallStartsAsOneToOneAgain) {
  CallGroupLook look;
  EXPECT_TRUE(look.Observe("c1", false, 3));
  EXPECT_FALSE(look.Observe("c2", false, 2)) << "a different call id forgets";
  EXPECT_FALSE(look.Observe("c2", false, 1));
  EXPECT_TRUE(look.Observe("c2", false, 3));
}

TEST(CallGroupLookTest, TheNameIsOfSomeoneStillJoined) {
  // The store lists "ann" (who left) first, but only the local person and "bo" are joined now.
  const std::vector<CallScreen::Member> joined{Member("me", true), Member("bo")};
  const CallScreen::Member* remote = FirstJoinedRemote(joined);
  ASSERT_NE(remote, nullptr);
  EXPECT_EQ(remote->identity, "bo");
}

TEST(CallGroupLookTest, NobodyElseJoinedGivesNoName) {
  EXPECT_EQ(FirstJoinedRemote({Member("me", true)}), nullptr);
  EXPECT_EQ(FirstJoinedRemote({}), nullptr);
}

} // namespace
} // namespace pbr
