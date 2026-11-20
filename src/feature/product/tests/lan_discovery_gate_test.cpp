// LAN discovery waits for the end of account setup (iOS's local-network alert must not cover the setup page), starts
// once when setup finishes, starts at once for an account that already finished, and never twice.
#include "feature/product/LanDiscoveryGate.h"

#include <gtest/gtest.h>

#include <string>
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
} // namespace pbr
