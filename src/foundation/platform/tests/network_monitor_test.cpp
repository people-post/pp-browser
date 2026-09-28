#include "foundation/platform/NetworkMonitor.h"
#include "foundation/platform/NetworkMonitorBackend.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <vector>

namespace pbr {
namespace {

NetworkState Wifi(const std::string& fingerprint) {
  NetworkState state;
  state.online = true;
  state.transport = NetworkTransport::Wifi;
  state.fingerprint = fingerprint;
  return state;
}

class NetworkMonitorTest : public ::testing::Test {
protected:
  void StartCollecting() {
    EXPECT_FALSE(monitor_.Start([this](const NetworkChange& change) { changes_.push_back(change); }))
        << "no OS backend in this monitor";
  }

  NetworkMonitor monitor_{/*with_os_backend=*/false};
  std::vector<NetworkChange> changes_;
};

// The first state is the baseline: reported as generation 0 (consumers classify from it), not a change.
TEST_F(NetworkMonitorTest, FirstStateIsTheBaseline) {
  StartCollecting();
  monitor_.OnState(Wifi("wlan0/192.168.1.5"));
  ASSERT_EQ(changes_.size(), 1u);
  EXPECT_EQ(changes_[0].generation, 0u);
  EXPECT_EQ(changes_[0].current, Wifi("wlan0/192.168.1.5"));
  EXPECT_EQ(changes_[0].previous, NetworkState{});
  EXPECT_EQ(monitor_.Current(), Wifi("wlan0/192.168.1.5"));
  EXPECT_EQ(monitor_.Generation(), 0u);
}

// Wi-Fi → cellular: one change carrying both states; generations count changes.
TEST_F(NetworkMonitorTest, MaterialChangesAreReportedWithGenerations) {
  StartCollecting();
  monitor_.OnState(Wifi("wlan0/192.168.1.5"));
  NetworkState cell;
  cell.online = true;
  cell.transport = NetworkTransport::Cellular;
  cell.expensive = true;
  cell.fingerprint = "rmnet0/10.20.30.40";
  monitor_.OnState(cell);
  monitor_.OnState(Wifi("wlan0/192.168.1.9"));  // back on Wi-Fi, new address

  ASSERT_EQ(changes_.size(), 3u) << "baseline + two changes";
  EXPECT_EQ(changes_[1].generation, 1u);
  EXPECT_EQ(changes_[1].previous, Wifi("wlan0/192.168.1.5"));
  EXPECT_EQ(changes_[1].current, cell);
  EXPECT_EQ(changes_[2].generation, 2u);
  EXPECT_EQ(changes_[2].current.fingerprint, "wlan0/192.168.1.9");
  EXPECT_EQ(monitor_.Generation(), 2u);
}

// OS sources fire bursts for one change (and for unrelated interfaces): identical states are no event.
TEST_F(NetworkMonitorTest, RepeatedStatesAreNotChanges) {
  StartCollecting();
  for (int i = 0; i < 5; ++i) {
    monitor_.OnState(Wifi("wlan0/192.168.1.5"));
  }
  EXPECT_EQ(changes_.size(), 1u) << "the baseline only";
}

TEST_F(NetworkMonitorTest, NothingIsReportedAfterStop) {
  StartCollecting();
  monitor_.OnState(Wifi("a"));
  monitor_.Stop();
  monitor_.OnState(Wifi("b"));
  EXPECT_EQ(changes_.size(), 1u) << "the baseline only";
  monitor_.Stop();  // idempotent
}

TEST(NetworkMonitorParseTest, ProcNetRouteDefaultIfaces) {
  const char* text =
      "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n"
      "wlp2s0\t00000000\t0101A8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0\n"
      "wlp2s0\t0001A8C0\t00000000\t0001\t0\t0\t600\t00FFFFFF\t0\t0\t0\n"
      "docker0\t000011AC\t00000000\t0001\t0\t0\t0\t0000FFFF\t0\t0\t0\n"
      "down0\t00000000\t0101A8C0\t0002\t0\t0\t100\t00000000\t0\t0\t0\n";
  EXPECT_EQ(detail::ParseProcNetRouteDefaultIfaces(text), std::vector<std::string>{"wlp2s0"})
      << "only UP default routes; docker's subnet route is not a default";
}

TEST(NetworkMonitorParseTest, ProcNetIpv6RouteDefaultIfaces) {
  const std::string any(32, '0');
  const std::string text =
      any + " 00 " + any + " 00 fe800000000000000000000000000001 00000400 00000001 00000000 00450003   wlp2s0\n" +
      "20010db8000000000000000000000000 40 " + any + " 00 " + any + " 00000100 00000001 00000000 00000001   wlp2s0\n" +
      any + " 00 " + any + " 00 " + any + " ffffffff 00000001 00000000 00200200       lo\n";
  EXPECT_EQ(detail::ParseProcNetIpv6RouteDefaultIfaces(text), std::vector<std::string>{"wlp2s0"})
      << "the ::/0 route, not the /64, and never lo's unreachable default";
}

TEST(NetworkMonitorParseTest, FingerprintsAreOrderIndependent) {
  EXPECT_EQ(detail::JoinSorted({"b/2", "a/1", "b/2"}), "a/1,b/2");
  EXPECT_EQ(detail::JoinSorted({}), "");
}

// The desktop OS backend starts and stops cleanly (Stop waits out its thread / queue / callbacks).
TEST(NetworkMonitorOsTest, OsBackendStartsAndStops) {
  NetworkMonitor monitor;
  ASSERT_TRUE(monitor.Start([](const NetworkChange&) {}));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  monitor.Stop();
  monitor.Stop();
  EXPECT_TRUE(monitor.Start([](const NetworkChange&) {})) << "restartable";
  monitor.Stop();
}

} // namespace
} // namespace pbr
