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
  const auto snap = pbr::BuildStatusbarClusterSnapshot(false, false, false,
                                                       pbr::ReachabilityStatus::Reachable, true);
  EXPECT_EQ(snap.mesh, pbr::StatusbarClusterSnapshot::MeshState::Hidden);
  EXPECT_EQ(snap.reach, pbr::StatusbarClusterSnapshot::ReachState::Hidden);
  EXPECT_FALSE(snap.help_visible);
  EXPECT_TRUE(snap.label.empty());
}

TEST_F(StatusbarClusterTest, DirectOffWhenHostDown) {
  const auto snap = pbr::BuildStatusbarClusterSnapshot(true, false, false,
                                                       pbr::ReachabilityStatus::Unknown, false);
  EXPECT_EQ(snap.mesh, pbr::StatusbarClusterSnapshot::MeshState::Off);
  EXPECT_EQ(snap.reach, pbr::StatusbarClusterSnapshot::ReachState::Hidden);
  EXPECT_FALSE(snap.help_visible);
  EXPECT_FALSE(snap.label.empty());
  EXPECT_EQ(snap.label_tone, pbr::StatusbarClusterSnapshot::LabelTone::Muted);
}

TEST_F(StatusbarClusterTest, MeshErrorWhenLibp2pFailed) {
  const auto snap = pbr::BuildStatusbarClusterSnapshot(true, false, true,
                                                       pbr::ReachabilityStatus::Unknown, true);
  EXPECT_EQ(snap.mesh, pbr::StatusbarClusterSnapshot::MeshState::Error);
  EXPECT_TRUE(snap.help_visible);
  EXPECT_TRUE(snap.label.empty());
  EXPECT_EQ(snap.label_tone, pbr::StatusbarClusterSnapshot::LabelTone::Error);
}

TEST_F(StatusbarClusterTest, ReachableHealthyHasNoLabel) {
  const auto snap = pbr::BuildStatusbarClusterSnapshot(true, true, false,
                                                       pbr::ReachabilityStatus::Reachable, true);
  EXPECT_EQ(snap.mesh, pbr::StatusbarClusterSnapshot::MeshState::On);
  EXPECT_EQ(snap.reach, pbr::StatusbarClusterSnapshot::ReachState::Reachable);
  EXPECT_TRUE(snap.help_visible);
  EXPECT_TRUE(snap.label.empty());
}

TEST_F(StatusbarClusterTest, OutboundOnlyWarnsWithSettingsParityLabel) {
  const auto snap =
      pbr::BuildStatusbarClusterSnapshot(true, true, false, pbr::ReachabilityStatus::OutboundOnly,
                                         false);
  EXPECT_EQ(snap.reach, pbr::StatusbarClusterSnapshot::ReachState::OutboundOnly);
  EXPECT_FALSE(snap.help_visible);
  EXPECT_EQ(snap.label_tone, pbr::StatusbarClusterSnapshot::LabelTone::Warn);
  EXPECT_FALSE(snap.label.empty());
}

TEST_F(StatusbarClusterTest, BlockedUsesErrorTone) {
  const auto snap = pbr::BuildStatusbarClusterSnapshot(true, true, false,
                                                       pbr::ReachabilityStatus::Blocked, true);
  EXPECT_EQ(snap.reach, pbr::StatusbarClusterSnapshot::ReachState::Blocked);
  EXPECT_EQ(snap.label_tone, pbr::StatusbarClusterSnapshot::LabelTone::Error);
  EXPECT_FALSE(snap.label.empty());
}

TEST_F(StatusbarClusterTest, CheckingMapsWithoutSparseLabel) {
  const auto snap = pbr::BuildStatusbarClusterSnapshot(true, true, false,
                                                       pbr::ReachabilityStatus::Checking, true);
  EXPECT_EQ(snap.reach, pbr::StatusbarClusterSnapshot::ReachState::Checking);
  EXPECT_TRUE(snap.label.empty());
}
