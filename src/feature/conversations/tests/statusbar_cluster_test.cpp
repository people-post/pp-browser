#include "foundation/i18n/LocalizationService.h"
#include "feature/conversations/MessagingShellPorts.h"

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
  EXPECT_FALSE(snap.direct_title.empty());
}

TEST_F(StatusbarClusterTest, PopoverClientSummary) {
  const auto snap = pbr::BuildStatusbarPopoverSnapshot(
      true, pbr::BriefRelayHealth::Ok, true, "", pbr::ReachabilityStatus::OutboundOnly, false, false,
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
      true, pbr::BriefRelayHealth::Ok, true, "dial failed", pbr::ReachabilityStatus::Reachable, true,
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
      false, pbr::BriefRelayHealth::Ok, true, "", pbr::ReachabilityStatus::Reachable, false, false,
      false, false);
  EXPECT_FALSE(snap.messaging_ready);
  EXPECT_TRUE(snap.brief_label.empty());
}

TEST_F(StatusbarClusterTest, LoadPillsWhenHelpingWithCounts) {
  pbr::RelayRuntimeStats load;
  load.circuit_serving = true;
  load.circuit.active_bridges = 2;
  load.media_serving = true;
  load.media.active_sessions = 1;
  load.media.active_participants = 3;
  const auto snap = pbr::BuildStatusbarClusterSnapshot(
      true, pbr::BriefRelayHealth::Ok, true, false, pbr::ReachabilityStatus::Reachable, true, load);
  EXPECT_TRUE(snap.load_circuit_visible);
  EXPECT_TRUE(snap.load_media_visible);
  EXPECT_FALSE(snap.load_circuit_label.empty());
  EXPECT_FALSE(snap.load_media_label.empty());
}

TEST_F(StatusbarClusterTest, LoadHiddenWhenCountsZero) {
  pbr::RelayRuntimeStats load;
  load.circuit_serving = true;
  load.media_serving = true;
  const auto snap = pbr::BuildStatusbarClusterSnapshot(
      true, pbr::BriefRelayHealth::Ok, true, false, pbr::ReachabilityStatus::Reachable, true, load);
  EXPECT_FALSE(snap.load_circuit_visible);
  EXPECT_FALSE(snap.load_media_visible);
}

TEST_F(StatusbarClusterTest, LoadHiddenForClientEvenWithCounts) {
  pbr::RelayRuntimeStats load;
  load.circuit_serving = true;
  load.circuit.active_bridges = 2;
  const auto snap = pbr::BuildStatusbarClusterSnapshot(
      true, pbr::BriefRelayHealth::Ok, true, false, pbr::ReachabilityStatus::Reachable, false, load);
  EXPECT_FALSE(snap.load_circuit_visible);
  EXPECT_FALSE(snap.help_visible);
}

TEST_F(StatusbarClusterTest, PopoverShowsLoadAggregates) {
  pbr::RelayRuntimeStats load;
  load.circuit_serving = true;
  load.circuit.active_bridges = 1;
  load.media_serving = true;
  load.media.active_sessions = 2;
  load.media.active_participants = 4;
  const auto snap = pbr::BuildStatusbarPopoverSnapshot(
      true, pbr::BriefRelayHealth::Ok, true, "", pbr::ReachabilityStatus::Reachable, false, true, false,
      true, load);
  EXPECT_TRUE(snap.show_load);
  EXPECT_EQ(snap.circuit_bridges, 1u);
  EXPECT_EQ(snap.media_sessions, 2u);
  EXPECT_EQ(snap.media_participants, 4u);
  EXPECT_FALSE(snap.circuit_load_label.empty());
}

namespace {

pbr::MeshTrafficTotals TrafficAt(std::chrono::steady_clock::time_point at) {
  pbr::MeshTrafficTotals totals;
  totals.available = true;
  totals.links = 3;
  totals.at = at;
  return totals;
}

} // namespace

TEST_F(StatusbarClusterTest, TrafficRatesAreDeltasOverTheInterval) {
  const auto t0 = std::chrono::steady_clock::now();
  pbr::MeshTrafficTotals before = TrafficAt(t0);
  before.sent_bytes = 1000;
  before.received_bytes = 500;
  before.reliable_sent = 100;
  before.retransmits = 1;
  before.rtt_samples = 10;
  before.rtt_sum_ms = 400;
  before.relayed_bytes = 0;
  pbr::MeshTrafficTotals now = TrafficAt(t0 + std::chrono::seconds(2));
  now.sent_bytes = 5000;
  now.received_bytes = 2500;
  now.reliable_sent = 200;
  now.retransmits = 6;
  now.rtt_samples = 14;
  now.rtt_sum_ms = 600;
  now.relayed_bytes = 4096;

  const pbr::MeshTrafficRates rates = pbr::MeshTrafficRatesBetween(before, now);
  ASSERT_TRUE(rates.valid);
  EXPECT_DOUBLE_EQ(rates.sent_bps, 2000.0);
  EXPECT_DOUBLE_EQ(rates.received_bps, 1000.0);
  EXPECT_DOUBLE_EQ(rates.relayed_bps, 2048.0);
  EXPECT_EQ(rates.rtt_ms, 50);  // 200 ms over 4 samples
  EXPECT_DOUBLE_EQ(rates.resend_pct, 5.0);
}

TEST_F(StatusbarClusterTest, TrafficRatesWithoutSamplesOrAfterRestart) {
  const auto t0 = std::chrono::steady_clock::now();
  pbr::MeshTrafficTotals before = TrafficAt(t0);
  before.sent_bytes = 9000;  // the mesh restarted: counters went back
  pbr::MeshTrafficTotals now = TrafficAt(t0 + std::chrono::seconds(1));
  now.sent_bytes = 100;

  const pbr::MeshTrafficRates rates = pbr::MeshTrafficRatesBetween(before, now);
  ASSERT_TRUE(rates.valid);
  EXPECT_DOUBLE_EQ(rates.sent_bps, 0.0);
  EXPECT_EQ(rates.rtt_ms, -1);
  EXPECT_LT(rates.resend_pct, 0.0);

  EXPECT_FALSE(pbr::MeshTrafficRatesBetween(pbr::MeshTrafficTotals{}, now).valid);
  EXPECT_FALSE(pbr::MeshTrafficRatesBetween(now, now).valid);
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
      true, pbr::BriefRelayHealth::Ok, true, "", pbr::ReachabilityStatus::Reachable, false, true, false,
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
                                                 pbr::ReachabilityStatus::Reachable, false, true, false, true,
                                                 {}, traffic);
  EXPECT_FALSE(snap.relay_rate_label.empty());
  EXPECT_TRUE(snap.show_load);

  snap = pbr::BuildStatusbarPopoverSnapshot(true, pbr::BriefRelayHealth::Ok, true, "",
                                            pbr::ReachabilityStatus::Reachable, false, true, false, false, {},
                                            traffic);
  EXPECT_TRUE(snap.relay_rate_label.empty());
}

TEST_F(StatusbarClusterTest, PopoverHidesTrafficWhenMeshDown) {
  pbr::MeshTrafficView traffic;
  traffic.available = true;
  const auto snap = pbr::BuildStatusbarPopoverSnapshot(
      true, pbr::BriefRelayHealth::Ok, false, "", pbr::ReachabilityStatus::Unknown, false, false, false,
      false, {}, traffic);
  EXPECT_FALSE(snap.show_network);
}
