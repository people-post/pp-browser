#include "foundation/i18n/LocalizationService.h"
#include "gui/shell/StatusbarText.h"

#include <chrono>

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
                                         pp::mesh::ReachabilityStatus::Reachable, true);
  EXPECT_EQ(snap.brief, pbr::StatusbarClusterSnapshot::BriefState::Hidden);
  EXPECT_EQ(snap.direct, pbr::StatusbarClusterSnapshot::DirectState::Hidden);
  EXPECT_EQ(snap.inbound, pbr::StatusbarClusterSnapshot::InboundState::Hidden);
  EXPECT_FALSE(snap.help_visible);
  EXPECT_TRUE(snap.label.empty());
}

TEST_F(StatusbarClusterTest, ClientShowsBriefAndDirectOff) {
  const auto snap =
      pbr::BuildStatusbarClusterSnapshot(true, pbr::BriefRelayHealth::Unknown, false, false,
                                         pp::mesh::ReachabilityStatus::Unknown, false);
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
                                         pp::mesh::ReachabilityStatus::Unknown, false);
  EXPECT_EQ(snap.brief, pbr::StatusbarClusterSnapshot::BriefState::Failed);
  EXPECT_EQ(snap.direct, pbr::StatusbarClusterSnapshot::DirectState::Off);
  EXPECT_EQ(snap.label_tone, pbr::StatusbarClusterSnapshot::LabelTone::Error);
  EXPECT_FALSE(snap.label.empty());
}

TEST_F(StatusbarClusterTest, DirectOnWhenSeedDialOk) {
  const auto snap =
      pbr::BuildStatusbarClusterSnapshot(true, pbr::BriefRelayHealth::Ok, true, false,
                                         pp::mesh::ReachabilityStatus::OutboundOnly, false);
  EXPECT_EQ(snap.brief, pbr::StatusbarClusterSnapshot::BriefState::Ok);
  EXPECT_EQ(snap.direct, pbr::StatusbarClusterSnapshot::DirectState::On);
  EXPECT_EQ(snap.inbound, pbr::StatusbarClusterSnapshot::InboundState::Hidden);
  EXPECT_TRUE(snap.label.empty());
}

TEST_F(StatusbarClusterTest, NodeShowsInboundOffWhenOutboundOnly) {
  const auto snap =
      pbr::BuildStatusbarClusterSnapshot(true, pbr::BriefRelayHealth::Ok, true, false,
                                         pp::mesh::ReachabilityStatus::OutboundOnly, true);
  EXPECT_EQ(snap.direct, pbr::StatusbarClusterSnapshot::DirectState::On);
  EXPECT_EQ(snap.inbound, pbr::StatusbarClusterSnapshot::InboundState::Off);
  EXPECT_TRUE(snap.help_visible);
  EXPECT_EQ(snap.label_tone, pbr::StatusbarClusterSnapshot::LabelTone::Warn);
  EXPECT_FALSE(snap.label.empty());
}

TEST_F(StatusbarClusterTest, NodeShowsInboundOnWhenReachable) {
  const auto snap =
      pbr::BuildStatusbarClusterSnapshot(true, pbr::BriefRelayHealth::Ok, true, false,
                                         pp::mesh::ReachabilityStatus::Reachable, true);
  EXPECT_EQ(snap.direct, pbr::StatusbarClusterSnapshot::DirectState::On);
  EXPECT_EQ(snap.inbound, pbr::StatusbarClusterSnapshot::InboundState::On);
  EXPECT_TRUE(snap.label.empty());
}

TEST_F(StatusbarClusterTest, BlockedMarksDirectError) {
  const auto snap =
      pbr::BuildStatusbarClusterSnapshot(true, pbr::BriefRelayHealth::Ok, true, false,
                                         pp::mesh::ReachabilityStatus::Blocked, true);
  EXPECT_EQ(snap.direct, pbr::StatusbarClusterSnapshot::DirectState::Error);
  EXPECT_EQ(snap.inbound, pbr::StatusbarClusterSnapshot::InboundState::Off);
  EXPECT_EQ(snap.label_tone, pbr::StatusbarClusterSnapshot::LabelTone::Error);
}

TEST_F(StatusbarClusterTest, CheckingMapsDirectChecking) {
  const auto snap =
      pbr::BuildStatusbarClusterSnapshot(true, pbr::BriefRelayHealth::Ok, true, false,
                                         pp::mesh::ReachabilityStatus::Checking, false);
  EXPECT_EQ(snap.direct, pbr::StatusbarClusterSnapshot::DirectState::Checking);
  EXPECT_EQ(snap.inbound, pbr::StatusbarClusterSnapshot::InboundState::Hidden);
  EXPECT_TRUE(snap.label.empty());
  EXPECT_FALSE(snap.direct_title.empty());
}

TEST_F(StatusbarClusterTest, PopoverClientSummary) {
  const auto snap = pbr::BuildStatusbarPopoverSnapshot(
      true, pbr::BriefRelayHealth::Ok, true, "", pp::mesh::ReachabilityStatus::OutboundOnly, false, false,
      false, false);
  EXPECT_TRUE(snap.messaging_ready);
  EXPECT_FALSE(snap.brief_label.empty());
  EXPECT_FALSE(snap.direct_label.empty());
  EXPECT_FALSE(snap.reachability_status_label.empty());
  EXPECT_FALSE(snap.reachability_summary.empty());
  EXPECT_FALSE(snap.help_visible);
  EXPECT_FALSE(snap.show_upnp);
  EXPECT_TRUE(snap.last_error.empty());
}

TEST_F(StatusbarClusterTest, PopoverNodeShowsHelpAndUpnp) {
  const auto snap = pbr::BuildStatusbarPopoverSnapshot(
      true, pbr::BriefRelayHealth::Ok, true, "dial failed", pp::mesh::ReachabilityStatus::Reachable, true,
      true, true, true);
  EXPECT_TRUE(snap.help_visible);
  EXPECT_FALSE(snap.help_label.empty());
  EXPECT_TRUE(snap.show_upnp);
  EXPECT_TRUE(snap.upnp_mapped);
  EXPECT_FALSE(snap.upnp_label.empty());
  EXPECT_EQ(snap.last_error, "dial failed");
}

TEST_F(StatusbarClusterTest, PopoverHiddenWhenMessagingNotReady) {
  const auto snap = pbr::BuildStatusbarPopoverSnapshot(
      false, pbr::BriefRelayHealth::Ok, true, "", pp::mesh::ReachabilityStatus::Reachable, false, false,
      false, false);
  EXPECT_FALSE(snap.messaging_ready);
  EXPECT_TRUE(snap.brief_label.empty());
}

TEST_F(StatusbarClusterTest, LoadPillsWhenHelpingWithCounts) {
  pp::mesh::RelayRuntimeStats load;
  load.circuit_serving = true;
  load.circuit.active_bridges = 2;
  load.media_serving = true;
  load.media.active_sessions = 1;
  load.media.active_participants = 3;
  const auto snap = pbr::BuildStatusbarClusterSnapshot(
      true, pbr::BriefRelayHealth::Ok, true, false, pp::mesh::ReachabilityStatus::Reachable, true, load);
  EXPECT_TRUE(snap.load_circuit_visible);
  EXPECT_TRUE(snap.load_media_visible);
  EXPECT_FALSE(snap.load_circuit_label.empty());
  EXPECT_FALSE(snap.load_media_label.empty());
}

TEST_F(StatusbarClusterTest, LoadHiddenWhenCountsZero) {
  pp::mesh::RelayRuntimeStats load;
  load.circuit_serving = true;
  load.media_serving = true;
  const auto snap = pbr::BuildStatusbarClusterSnapshot(
      true, pbr::BriefRelayHealth::Ok, true, false, pp::mesh::ReachabilityStatus::Reachable, true, load);
  EXPECT_FALSE(snap.load_circuit_visible);
  EXPECT_FALSE(snap.load_media_visible);
}

TEST_F(StatusbarClusterTest, LoadHiddenForClientEvenWithCounts) {
  pp::mesh::RelayRuntimeStats load;
  load.circuit_serving = true;
  load.circuit.active_bridges = 2;
  const auto snap = pbr::BuildStatusbarClusterSnapshot(
      true, pbr::BriefRelayHealth::Ok, true, false, pp::mesh::ReachabilityStatus::Reachable, false, load);
  EXPECT_FALSE(snap.load_circuit_visible);
  EXPECT_FALSE(snap.help_visible);
}

TEST_F(StatusbarClusterTest, PopoverShowsLoadAggregates) {
  pp::mesh::RelayRuntimeStats load;
  load.circuit_serving = true;
  load.circuit.active_bridges = 1;
  load.media_serving = true;
  load.media.active_sessions = 2;
  load.media.active_participants = 4;
  const auto snap = pbr::BuildStatusbarPopoverSnapshot(
      true, pbr::BriefRelayHealth::Ok, true, "", pp::mesh::ReachabilityStatus::Reachable, false, true, false,
      true, load);
  EXPECT_TRUE(snap.show_load);
  EXPECT_EQ(snap.circuit_bridges, 1u);
  EXPECT_EQ(snap.media_sessions, 2u);
  EXPECT_EQ(snap.media_participants, 4u);
  EXPECT_FALSE(snap.circuit_load_label.empty());
}

TEST_F(StatusbarClusterTest, PopoverShowsMeshTraffic) {
  pbr::MeshTrafficView traffic;
  traffic.available = true;
  traffic.links = 3;
  traffic.rates.valid = true;
  traffic.rates.sent_bps = 2048;
  traffic.rates.received_bps = 1024;
  traffic.rates.rtt_ms = 42;
  traffic.rates.resend_pct = 1.5;
  const auto snap = pbr::BuildStatusbarPopoverSnapshot(
      true, pbr::BriefRelayHealth::Ok, true, "", pp::mesh::ReachabilityStatus::Reachable, false, true, false,
      false, {}, traffic);
  EXPECT_TRUE(snap.show_network);
  EXPECT_FALSE(snap.network_links_label.empty());
  EXPECT_FALSE(snap.network_rate_label.empty());
  EXPECT_FALSE(snap.network_rtt_label.empty());
  EXPECT_FALSE(snap.network_resend_label.empty());
  EXPECT_TRUE(snap.relay_rate_label.empty());
  EXPECT_FALSE(snap.show_load);
}

TEST_F(StatusbarClusterTest, PopoverRelayRateOnlyWhenHelping) {
  pbr::MeshTrafficView traffic;
  traffic.available = true;
  traffic.rates.valid = true;
  traffic.rates.relayed_bps = 50000;
  auto snap = pbr::BuildStatusbarPopoverSnapshot(true, pbr::BriefRelayHealth::Ok, true, "",
                                                 pp::mesh::ReachabilityStatus::Reachable, false, true, false, true,
                                                 {}, traffic);
  EXPECT_FALSE(snap.relay_rate_label.empty());
  EXPECT_TRUE(snap.show_load);

  snap = pbr::BuildStatusbarPopoverSnapshot(true, pbr::BriefRelayHealth::Ok, true, "",
                                            pp::mesh::ReachabilityStatus::Reachable, false, true, false, false, {},
                                            traffic);
  EXPECT_TRUE(snap.relay_rate_label.empty());
}

TEST_F(StatusbarClusterTest, PopoverShowsRendezvousParking) {
  pbr::MeshTrafficView traffic;
  traffic.available = true;
  traffic.parked_relays = 2;
  auto snap = pbr::BuildStatusbarPopoverSnapshot(true, pbr::BriefRelayHealth::Ok, true, "",
                                                 pp::mesh::ReachabilityStatus::OutboundOnly, false, false, false,
                                                 false, {}, traffic);
  EXPECT_FALSE(snap.network_parked_label.empty());
  const std::string parked = snap.network_parked_label;

  // Behind NAT with no relay parked: say dialers have no way in.
  traffic.parked_relays = 0;
  snap = pbr::BuildStatusbarPopoverSnapshot(true, pbr::BriefRelayHealth::Ok, true, "",
                                            pp::mesh::ReachabilityStatus::OutboundOnly, false, false, false, false,
                                            {}, traffic);
  EXPECT_FALSE(snap.network_parked_label.empty());
  EXPECT_NE(snap.network_parked_label, parked);

  // Directly reachable: parking does not matter, nothing to say.
  snap = pbr::BuildStatusbarPopoverSnapshot(true, pbr::BriefRelayHealth::Ok, true, "",
                                            pp::mesh::ReachabilityStatus::Reachable, false, true, false, false, {},
                                            traffic);
  EXPECT_TRUE(snap.network_parked_label.empty());
}

TEST_F(StatusbarClusterTest, PopoverHidesTrafficWhenMeshDown) {
  pbr::MeshTrafficView traffic;
  traffic.available = true;
  const auto snap = pbr::BuildStatusbarPopoverSnapshot(
      true, pbr::BriefRelayHealth::Ok, false, "", pp::mesh::ReachabilityStatus::Unknown, false, false, false,
      false, {}, traffic);
  EXPECT_FALSE(snap.show_network);
}

