// The status bar popover's traffic rates: deltas between samples (MeshTrafficSampler).
#include "feature/conversations/MessagingShellPorts.h"

#include <chrono>

#include <gtest/gtest.h>

namespace {

pbr::MeshTrafficTotals TrafficAt(std::chrono::steady_clock::time_point at) {
  pbr::MeshTrafficTotals totals;
  totals.available = true;
  totals.links = 3;
  totals.at = at;
  return totals;
}

} // namespace

TEST(MeshTrafficRatesTest, TrafficRatesAreDeltasOverTheInterval) {
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

TEST(MeshTrafficRatesTest, TrafficRatesWithoutSamplesOrAfterRestart) {
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

TEST(MeshTrafficRatesTest, TrafficSamplerStartsOverAfterAGap) {
  const auto t0 = std::chrono::steady_clock::now();
  pbr::MeshTrafficSampler sampler;
  pbr::MeshTrafficTotals a = TrafficAt(t0);
  EXPECT_FALSE(sampler.Sample(a).valid) << "first sample";

  pbr::MeshTrafficTotals b = TrafficAt(t0 + std::chrono::seconds(2));
  b.sent_bytes = 2000;
  const pbr::MeshTrafficRates rates = sampler.Sample(b);
  ASSERT_TRUE(rates.valid);
  EXPECT_DOUBLE_EQ(rates.sent_bps, 1000.0);

  pbr::MeshTrafficTotals c = TrafficAt(t0 + std::chrono::milliseconds(2500));
  c.sent_bytes = 9000;
  EXPECT_DOUBLE_EQ(sampler.Sample(c).sent_bps, 1000.0) << "under a second: the last rates stand";

  // The popover was closed for ten minutes: no rate averaged over the gap.
  pbr::MeshTrafficTotals d = TrafficAt(t0 + std::chrono::minutes(10));
  d.sent_bytes = 5000000;
  EXPECT_FALSE(sampler.Sample(d).valid);
  pbr::MeshTrafficTotals e = TrafficAt(t0 + std::chrono::minutes(10) + std::chrono::seconds(1));
  e.sent_bytes = 5000500;
  EXPECT_DOUBLE_EQ(sampler.Sample(e).sent_bps, 500.0);
}
