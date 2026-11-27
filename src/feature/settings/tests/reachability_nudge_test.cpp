#include "feature/settings/ReachabilityNudge.h"

#include <gtest/gtest.h>

namespace {

TEST(ReachabilityNudgeTest, SeverityOrdersBlockedAboveOutboundOnly) {
  EXPECT_EQ(pbr::ReachabilityNudgeSeverity(""), 0);
  EXPECT_EQ(pbr::ReachabilityNudgeSeverity("reachable"), 0);
  EXPECT_LT(pbr::ReachabilityNudgeSeverity("outbound_only"), pbr::ReachabilityNudgeSeverity("blocked"));
}

} // namespace
