#include "base/i18n/LocalizationService.h"
#include "feature/messaging/MessagingShellPorts.h"

#include <gtest/gtest.h>

namespace {

class StatusbarClusterTest : public ::testing::Test {
protected:
  void SetUp() override {
#ifdef PP_BROWSER_ASSETS_DIR
    ASSERT_TRUE(pbr::LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
#endif
    pbr::LocalizationService::Instance().SetPreferredLanguage("en");
  }
};

} // namespace

TEST_F(StatusbarClusterTest, HiddenWhenMessagingNotReady) {
  const auto snap =
      pbr::BuildStatusbarClusterSnapshot(false, pbr::BriefRelayHealth::Ok, false, false,
                                         pbr::ReachabilityStatus::Reachable, true);
  EXPECT_EQ(snap.brief, pbr::StatusbarClusterSnapshot::BriefState::Hidden);
  EXPECT_EQ(snap.direct, pbr::StatusbarClusterSnapshot::DirectState::Hidden);
  EXPECT_EQ(snap.inbound, pbr::StatusbarClusterSnapshot::InboundState::Hidden);
  EXPECT_FALSE(snap.help_visible);
  EXPECT_TRUE(snap.label.empty());
}

TEST_F(StatusbarClusterTest, ClientShowsBriefAndDirectOff) {
  const auto snap =
      pbr::BuildStatusbarClusterSnapshot(true, pbr::BriefRelayHealth::Unknown, false, false,
                                         pbr::ReachabilityStatus::Unknown, false);
  EXPECT_EQ(snap.brief, pbr::StatusbarClusterSnapshot::BriefState::Unknown);
  EXPECT_EQ(snap.direct, pbr::StatusbarClusterSnapshot::DirectState::Off);
  EXPECT_EQ(snap.inbound, pbr::StatusbarClusterSnapshot::InboundState::Hidden);
  EXPECT_FALSE(snap.help_visible);
  EXPECT_FALSE(snap.label.empty());
  EXPECT_EQ(snap.label_tone, pbr::StatusbarClusterSnapshot::LabelTone::Warn);
}

TEST_F(StatusbarClusterTest, BriefFailedTakesLabelPriority) {
  const auto snap =
      pbr::BuildStatusbarClusterSnapshot(true, pbr::BriefRelayHealth::Failed, false, false,
                                         pbr::ReachabilityStatus::Unknown, false);
  EXPECT_EQ(snap.brief, pbr::StatusbarClusterSnapshot::BriefState::Failed);
  EXPECT_EQ(snap.direct, pbr::StatusbarClusterSnapshot::DirectState::Off);
  EXPECT_EQ(snap.label_tone, pbr::StatusbarClusterSnapshot::LabelTone::Error);
  EXPECT_FALSE(snap.label.empty());
}

TEST_F(StatusbarClusterTest, DirectOnWhenSeedDialOk) {
  const auto snap =
      pbr::BuildStatusbarClusterSnapshot(true, pbr::BriefRelayHealth::Ok, true, false,
                                         pbr::ReachabilityStatus::OutboundOnly, false);
  EXPECT_EQ(snap.brief, pbr::StatusbarClusterSnapshot::BriefState::Ok);
  EXPECT_EQ(snap.direct, pbr::StatusbarClusterSnapshot::DirectState::On);
  EXPECT_EQ(snap.inbound, pbr::StatusbarClusterSnapshot::InboundState::Hidden);
  EXPECT_TRUE(snap.label.empty());
}

TEST_F(StatusbarClusterTest, NodeShowsInboundOffWhenOutboundOnly) {
  const auto snap =
      pbr::BuildStatusbarClusterSnapshot(true, pbr::BriefRelayHealth::Ok, true, false,
                                         pbr::ReachabilityStatus::OutboundOnly, true);
  EXPECT_EQ(snap.direct, pbr::StatusbarClusterSnapshot::DirectState::On);
  EXPECT_EQ(snap.inbound, pbr::StatusbarClusterSnapshot::InboundState::Off);
  EXPECT_TRUE(snap.help_visible);
  EXPECT_EQ(snap.label_tone, pbr::StatusbarClusterSnapshot::LabelTone::Warn);
  EXPECT_FALSE(snap.label.empty());
}

TEST_F(StatusbarClusterTest, NodeShowsInboundOnWhenReachable) {
  const auto snap =
      pbr::BuildStatusbarClusterSnapshot(true, pbr::BriefRelayHealth::Ok, true, false,
                                         pbr::ReachabilityStatus::Reachable, true);
  EXPECT_EQ(snap.direct, pbr::StatusbarClusterSnapshot::DirectState::On);
  EXPECT_EQ(snap.inbound, pbr::StatusbarClusterSnapshot::InboundState::On);
  EXPECT_TRUE(snap.label.empty());
}

TEST_F(StatusbarClusterTest, BlockedMarksDirectError) {
  const auto snap =
      pbr::BuildStatusbarClusterSnapshot(true, pbr::BriefRelayHealth::Ok, true, false,
                                         pbr::ReachabilityStatus::Blocked, true);
  EXPECT_EQ(snap.direct, pbr::StatusbarClusterSnapshot::DirectState::Error);
  EXPECT_EQ(snap.inbound, pbr::StatusbarClusterSnapshot::InboundState::Off);
  EXPECT_EQ(snap.label_tone, pbr::StatusbarClusterSnapshot::LabelTone::Error);
}

TEST_F(StatusbarClusterTest, CheckingMapsDirectChecking) {
  const auto snap =
      pbr::BuildStatusbarClusterSnapshot(true, pbr::BriefRelayHealth::Ok, true, false,
                                         pbr::ReachabilityStatus::Checking, false);
  EXPECT_EQ(snap.direct, pbr::StatusbarClusterSnapshot::DirectState::Checking);
  EXPECT_EQ(snap.inbound, pbr::StatusbarClusterSnapshot::InboundState::Hidden);
  EXPECT_TRUE(snap.label.empty());
}
