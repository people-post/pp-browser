#include "domain/messaging/CallMediaStatusLogic.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// V037: a path planner's progress shows as the call's media Status; idle / stopping change nothing.
TEST(CallMediaStatusLogicTest, PlannerProgressMapsToStatus) {
  EXPECT_EQ(MediaStatusForHopProgress(CallHopPlannerPhase::WaitingAttach), CallMediaStatus::HopWaiting);
  EXPECT_EQ(MediaStatusForHopProgress(CallHopPlannerPhase::Attaching), CallMediaStatus::HopAttaching);
  EXPECT_EQ(MediaStatusForHopProgress(CallHopPlannerPhase::Live), CallMediaStatus::HopLive);
  EXPECT_EQ(MediaStatusForHopProgress(CallHopPlannerPhase::Migrating), CallMediaStatus::Migrating);
  EXPECT_EQ(MediaStatusForHopProgress(CallHopPlannerPhase::Idle), std::nullopt);
  EXPECT_EQ(MediaStatusForHopProgress(CallHopPlannerPhase::Stopping), std::nullopt);

  for (const auto phase : {CallDirectPlannerPhase::Arming, CallDirectPlannerPhase::KeyWait,
                           CallDirectPlannerPhase::Connecting}) {
    EXPECT_EQ(MediaStatusForDirectProgress(phase), CallMediaStatus::DirectConnecting);
  }
  EXPECT_EQ(MediaStatusForDirectProgress(CallDirectPlannerPhase::DegradedTxOnly), CallMediaStatus::DegradedTxOnly);
  EXPECT_EQ(MediaStatusForDirectProgress(CallDirectPlannerPhase::Reconnecting), CallMediaStatus::Reconnecting);
  EXPECT_EQ(MediaStatusForDirectProgress(CallDirectPlannerPhase::Live), std::nullopt)
      << "DirectConnected moves the call to DirectLive, with its phase";
}

// A SoftMigrate arms only from a 1:1 or undecided Status — never over a hop already in progress.
TEST(CallMediaStatusLogicTest, SoftMigrateArmsFromDirectStatusesOnly) {
  for (const auto status : {CallMediaStatus::None, CallMediaStatus::Deciding, CallMediaStatus::DirectConnecting,
                            CallMediaStatus::DirectLive, CallMediaStatus::DegradedTxOnly}) {
    EXPECT_TRUE(SoftMigrateMayArm(status)) << static_cast<int>(status);
  }
  for (const auto status : {CallMediaStatus::HopWaiting, CallMediaStatus::HopAttaching, CallMediaStatus::HopLive,
                            CallMediaStatus::Migrating, CallMediaStatus::Failed, CallMediaStatus::Reconnecting}) {
    EXPECT_FALSE(SoftMigrateMayArm(status)) << static_cast<int>(status);
  }
}

TEST(CallMediaStatusLogicTest, DirectArmingRequestHonoredDuringSetUpOnly) {
  for (const auto phase : {CallPhase::Accepting, CallPhase::JoinedLocal, CallPhase::MediaPending,
                           CallPhase::MediaConnecting}) {
    EXPECT_TRUE(ShouldHonorDirectArmingRequest(phase)) << static_cast<int>(phase);
  }
  for (const auto phase : {CallPhase::Idle, CallPhase::Ringing, CallPhase::OutboundCalling, CallPhase::InCall,
                           CallPhase::ConnectFailed}) {
    EXPECT_FALSE(ShouldHonorDirectArmingRequest(phase)) << static_cast<int>(phase);
  }
}

} // namespace
} // namespace pbr
