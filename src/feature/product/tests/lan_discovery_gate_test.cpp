// LAN discovery waits for the end of account setup (iOS's local-network alert must not cover the setup page), starts
// once when setup finishes, starts at once for an account that already finished, and never twice.
#include "feature/product/LanDiscoveryGate.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace pbr {
namespace {

struct Recorder {
  std::vector<std::string> log;
  LanDiscoveryGate gate{{[this](const std::string& peer, const int port) {
                           log.push_back("advertise " + peer + ":" + std::to_string(port));
                         },
                         [this]() { log.push_back("stop"); }}};
  void Request() {
    gate.Request([this]() { log.push_back("start"); });
  }
};

TEST(LanDiscoveryGateTest, NothingStartsWhileSetupIsNotFinished) {
  Recorder r;
  r.gate.SetAllowed(false);
  r.Request();
  r.gate.Advertise("peer", 4000);
  EXPECT_TRUE(r.log.empty());
  EXPECT_FALSE(r.gate.Started());
}

TEST(LanDiscoveryGateTest, FinishingSetupStartsItOnceWithTheLatestAdvertisement) {
  Recorder r;
  r.gate.SetAllowed(false);
  r.Request();
  r.gate.Advertise("old", 1);
  r.gate.Advertise("peer", 4000);
  r.gate.SetAllowed(true);
  EXPECT_EQ(r.log, (std::vector<std::string>{"start", "advertise peer:4000"}));
  r.gate.SetAllowed(true); // the wizard's end can be told again
  EXPECT_EQ(r.log.size(), 2u) << "no double start";
}

TEST(LanDiscoveryGateTest, AnAccountAlreadySetUpStartsAtOnceAsBefore) {
  Recorder r;
  r.gate.SetAllowed(true);
  r.Request();
  EXPECT_EQ(r.log, (std::vector<std::string>{"start"}));
  r.gate.Advertise("peer", 4000);
  EXPECT_EQ(r.log.back(), "advertise peer:4000") << "announced at once";
  r.gate.SetAllowed(true);
  EXPECT_EQ(r.log.size(), 2u);
}

TEST(LanDiscoveryGateTest, AnOpenGateFromTheStartNeedsNoWord) {
  Recorder r; // default: open (a node, a probe, the tests)
  r.Request();
  EXPECT_EQ(r.log, (std::vector<std::string>{"start"}));
}

TEST(LanDiscoveryGateTest, StoppingWhileHeldForgetsTheRequestAndLaterOpeningStartsNothing) {
  Recorder r;
  r.gate.SetAllowed(false);
  r.Request();
  r.gate.Cancel();
  r.gate.SetAllowed(true);
  EXPECT_TRUE(r.log.empty());
}

TEST(LanDiscoveryGateTest, ARestartOfTheMeshStopsTheRunningOneAndStartsTheNew) {
  Recorder r;
  r.Request();
  r.Request(); // StopLanDiscovery then StartLanDiscovery, as the hub does on a mesh restart
  EXPECT_EQ(r.log, (std::vector<std::string>{"start", "stop", "start"}));
}

} // namespace
// The callables run unlocked (mDNS start can block on the iOS alert): the gate stays usable meanwhile.
TEST(LanDiscoveryGateTest, ACallFromInsideTheStartDoesNotDeadlockAndTheGateIsNotHeldDuringIt) {
  Recorder r;
  r.gate.Request([&r]() {
    r.log.push_back("start");
    r.gate.Advertise("inside", 7); // would deadlock if the start ran under the gate's mutex
    EXPECT_TRUE(r.gate.Started());
  });
  EXPECT_EQ(r.log, (std::vector<std::string>{"start", "advertise inside:7"})) << "after the start returns, in order";
}

TEST(LanDiscoveryGateTest, StopRequestedWhileStartingRunsRightAfterTheStartReturns) {
  Recorder r;
  r.gate.Request([&r]() {
    r.log.push_back("start");
    r.gate.Advertise("peer", 1);
    r.gate.Cancel(); // returns at once; the stop waits for this start to finish
    r.log.push_back("start returns");
  });
  EXPECT_EQ(r.log, (std::vector<std::string>{"start", "start returns", "stop"}))
      << "stopped right after the start, and the advertisement of a cancelled start is dropped";
  EXPECT_FALSE(r.gate.Started());
  r.gate.SetAllowed(true);
  EXPECT_EQ(r.log.size(), 3u) << "nothing is held any more";
}

TEST(LanDiscoveryGateTest, TheLatestAdvertisementWinsWhenSeveralComeDuringTheStart) {
  Recorder r;
  r.gate.Request([&r]() {
    r.log.push_back("start");
    r.gate.Advertise("first", 1);
    r.gate.Advertise("second", 2);
  });
  EXPECT_EQ(r.log, (std::vector<std::string>{"start", "advertise second:2"}));
}

TEST(LanDiscoveryGateTest, ARequestFromInsideTheStartReplacesItAfterTheOldOneIsStopped) {
  Recorder r;
  r.gate.Request([&r]() {
    r.log.push_back("start 1");
    r.gate.Request([&r]() { r.log.push_back("start 2"); });
  });
  EXPECT_EQ(r.log, (std::vector<std::string>{"start 1", "stop", "start 2"}));
}

TEST(LanDiscoveryGateTest, AnotherThreadIsNotBlockedWhileTheStartWaitsOnTheAlert) {
  std::promise<void> in_start;
  std::promise<void> release;
  std::shared_future<void> released = release.get_future().share();
  std::atomic<int> advertised{0};
  std::atomic<int> stopped{0};
  LanDiscoveryGate gate{{[&advertised](const std::string&, int) { ++advertised; }, [&stopped]() { ++stopped; }}};
  std::thread starter([&]() {
    gate.Request([&]() {
      in_start.set_value();
      released.wait(); // the system alert is up
    });
  });
  in_start.get_future().wait();
  auto others = std::async(std::launch::async, [&]() {
    EXPECT_TRUE(gate.Started());
    gate.Advertise("peer", 9);
    gate.Cancel();
  });
  ASSERT_EQ(others.wait_for(std::chrono::seconds(5)), std::future_status::ready)
      << "another thread's calls must not wait for the start";
  EXPECT_EQ(stopped.load(), 0) << "the stop waits for the start to return";
  release.set_value();
  starter.join();
  EXPECT_EQ(stopped.load(), 1);
  EXPECT_EQ(advertised.load(), 0) << "cancelled: the advertisement is dropped";
}

} // namespace pbr
