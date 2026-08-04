#include "base/messaging/HopHintLogic.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(HopHintLogicTest, CapSkipsFailedAndDedupes) {
  auto capped = CapGuestHopPreferences({"a", "failed", "a", "b", "c", "d", "e", "f"}, "failed", 5);
  ASSERT_EQ(capped.size(), 5u);
  EXPECT_EQ(capped[0], "a");
  EXPECT_EQ(capped[1], "b");
  EXPECT_EQ(capped[4], "e");
}

TEST(HopHintLogicTest, OwnerRepickOnIntersection) {
  const auto d = DecideHopHintOwnerAction({"phone", "seed", "desktop"}, {"org", "desktop", "seed"}, "phone");
  EXPECT_EQ(d.action, HopHintOwnerAction::RePick);
  EXPECT_EQ(d.preferred_hop_peer_id, "seed");
}

TEST(HopHintLogicTest, OwnerRefuseWhenNoIntersection) {
  const auto d = DecideHopHintOwnerAction({"phone-a", "phone-b"}, {"seed", "desktop"}, "old-hop");
  EXPECT_EQ(d.action, HopHintOwnerAction::RefuseGuest);
  EXPECT_TRUE(d.preferred_hop_peer_id.empty());
}

TEST(HopHintLogicTest, OwnerSkipsFailedHopEvenIfPreferred) {
  const auto d = DecideHopHintOwnerAction({"failed", "desktop"}, {"failed", "desktop"}, "failed");
  EXPECT_EQ(d.action, HopHintOwnerAction::RePick);
  EXPECT_EQ(d.preferred_hop_peer_id, "desktop");
}

} // namespace
} // namespace pbr
