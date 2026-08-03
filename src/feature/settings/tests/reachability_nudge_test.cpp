#include "feature/settings/ReachabilityNudge.h"

#include <gtest/gtest.h>

namespace {

TEST(ReachabilityNudgeTest, InactiveWithoutNodeOrBenignStatus) {
  EXPECT_FALSE(pbr::ReachabilityNudgeActive(false, "outbound_only", ""));
  EXPECT_FALSE(pbr::ReachabilityNudgeActive(true, "reachable", ""));
  EXPECT_FALSE(pbr::ReachabilityNudgeActive(true, "", ""));
}

TEST(ReachabilityNudgeTest, ShowsUntilAckedAtSameOrWorseSeverity) {
  EXPECT_TRUE(pbr::ReachabilityNudgeActive(true, "outbound_only", ""));
  EXPECT_FALSE(pbr::ReachabilityNudgeActive(true, "outbound_only", "outbound_only"));
  EXPECT_TRUE(pbr::ReachabilityNudgeActive(true, "blocked", "outbound_only"));
  EXPECT_FALSE(pbr::ReachabilityNudgeActive(true, "outbound_only", "blocked"));
  EXPECT_FALSE(pbr::ReachabilityNudgeActive(true, "blocked", "blocked"));
}

TEST(ReachabilityNudgeTest, StatusKeyMapsViewStatus) {
  EXPECT_EQ(pbr::ReachabilityNudgeStatusKey(pbr::SettingsReachabilityView::Status::OutboundOnly),
            "outbound_only");
  EXPECT_EQ(pbr::ReachabilityNudgeStatusKey(pbr::SettingsReachabilityView::Status::Blocked), "blocked");
  EXPECT_TRUE(
      pbr::ReachabilityNudgeStatusKey(pbr::SettingsReachabilityView::Status::Reachable).empty());
}

} // namespace
