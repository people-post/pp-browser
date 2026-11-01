#include "feature/calls/CallConnectDiag.h"

#include "foundation/diagnostics/DiagnosticsSelfCheck.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

const char* kCall = "call:ddc19c94-a3a1-4f57-b5e6-a22c198cb406";  // the failed call of #416

std::vector<std::string> IphoneAdvertised() {
  // One private v4 (carrier NAT range) and fifteen global v6, as the iPhone advertised on cellular.
  std::vector<std::string> mas = {"/ip4/10.0.0.7/udp/49733/adp/1.0.0/p2p/QmQ8NYJNCrpGNdzsafFpowL7Z5JzW1dSZzTuJfKP5piwVF"};
  for (int i = 1; i <= 15; ++i) {
    mas.push_back("/ip6/2001:db8::" + std::to_string(i) + "/udp/49733/adp/1.0.0/p2p/QmQ8NYJNCrpGNdzsafFpowL7Z5JzW1dSZzTuJfKP5piwVF");
  }
  return mas;
}

TEST(CallConnectDiagTest, CountsAddressesByFamilyAndClass) {
  const AddrCounts phone = CountAddrs(IphoneAdvertised());
  EXPECT_EQ(phone, (AddrCounts{1, 0, 15, 0}));
  // Android on the home Wi-Fi: a private v4 and link-local v6 only — no global address.
  const AddrCounts android = CountAddrs({"/ip4/192.168.0.105/udp/48569/adp/1.0.0", "/ip6/fe80::1/udp/48569/adp/1.0.0",
                                         "/ip6/fd00::2/udp/48569/adp/1.0.0", "/ip4/203.0.113.9/udp/1/adp/1.0.0",
                                         "/dns4/relay.example/udp/443"});
  EXPECT_EQ(android, (AddrCounts{1, 1, 0, 2}));
  EXPECT_EQ(CountAddrs({}), AddrCounts{});
}

TEST(CallConnectDiagTest, NetLineSaysWhatThisDeviceCouldOffer) {
  CallConnectEnv env;
  env.local = CountAddrs(IphoneAdvertised());
  env.attachment_known = true;
  env.attachment.online = true;
  env.attachment.cellular = true;
  env.relays_surface = 9;
  env.relays_connected = 4;
  env.relays_held_down = 2;
  EXPECT_EQ(CallNetLine(kCall, true, env).str(),
            "event=call.net call=ddc19c94 role=caller known=1 online=1 cellular=1 expensive=0 v4_private=1 v4_public=0 "
            "v6_global=15 v6_other=0 relays=9 relays_up=4 relays_down=2 peer_state=unknown peer_fail=none peer_fails=0");
}

TEST(CallConnectDiagTest, SignalLineCarriesThePeersAddressCountsAndTheWait) {
  EXPECT_EQ(CallSignalLine("invite_in", kCall, IphoneAdvertised(), 0, false).str(),
            "event=call.signal call=ddc19c94 phase=invite_in age_ms=0 implicit=0 v4_private=1 v4_public=0 v6_global=15 "
            "v6_other=0");
  EXPECT_EQ(CallSignalLine("accept_in", kCall, {"/ip4/192.168.0.105/udp/48569/adp/1.0.0"}, 6200, false).str(),
            "event=call.signal call=ddc19c94 phase=accept_in age_ms=6200 implicit=0 v4_private=1 v4_public=0 v6_global=0 "
            "v6_other=0");
  EXPECT_NE(CallSignalLine("accept_in", kCall, {}, -5, true).str().find("age_ms=0 implicit=1"), std::string::npos);
}

TEST(CallConnectDiagTest, ReachLinesNameTheKindOrTheReasonNotTheText) {
  EXPECT_EQ(CallReachOkLine(kCall, true, 2, mesh::PeerLinkKind::Punched, false, 6, 31000).str(),
            "event=call.reach call=ddc19c94 role=caller attempt=2 ok=1 kind=punched family=6 reused=0 ms=31000");
  const auto refused = mesh::PeerReachCoordinator::Failure::Of(
      mesh::PeerReachCoordinator::Err::CircuitFailed,
      "peer reach: circuit failed [circuit: circuit hop reach failed [tunnel: circuit-relay: refused [relay: circuit "
      "target peer endpoint not registered]]]");
  const std::string line = CallReachFailLine(kCall, true, 1, refused, 13500).str();
  EXPECT_EQ(line,
            "event=call.reach call=ddc19c94 role=caller attempt=1 ok=0 err=circuit_failed why=not_registered ms=13500");
  EXPECT_EQ(line.find("circuit-relay"), std::string::npos);
}

TEST(CallConnectDiagTest, WhyIsOneOfAFixedListFromTheMessageChain) {
  EXPECT_EQ(ReachWhyWord("... [tunnel: circuit-relay bridge timed out]").view(), "bridge_timeout");
  EXPECT_EQ(ReachWhyWord("circuit hop reach failed: nested timeout").view(), "nested_timeout");
  EXPECT_EQ(ReachWhyWord("EnsureViaCircuit nested miss").view(), "nested_miss");
  EXPECT_EQ(ReachWhyWord("handshake refused").view(), "refused");
  EXPECT_EQ(ReachWhyWord("dial timed out").view(), "timeout");
  EXPECT_EQ(ReachWhyWord("a message naming Alice").view(), "other");
  EXPECT_EQ(ReachWhyWord("").view(), "other");
}

TEST(CallConnectDiagTest, ResultLineOkAndGiveUp) {
  CallResultFacts ok;
  ok.offerer = false;
  ok.ok = true;
  ok.attempts = 1;
  ok.ms = 4200;
  ok.kind = mesh::PeerLinkKind::Direct;
  ok.family = 4;
  EXPECT_EQ(CallResultLine(kCall, ok, {}).str(),
            "event=call.result call=ddc19c94 role=callee ok=1 attempts=1 ms=4200 kind=direct family=4");

  CallResultFacts gave_up;
  gave_up.offerer = false;
  gave_up.attempts = 1;
  gave_up.ms = 31000;
  gave_up.media_err = CallMediaErr::Unreachable;
  gave_up.reach_failed = true;
  gave_up.reach_err = mesh::PeerReachCoordinator::Err::NotConnected;
  gave_up.why = ReachWhyWord("peer not connected");
  CallConnectEnv env;
  env.relays_surface = 7;
  env.relays_connected = 2;
  env.peer_state = mesh::PeerReachState::Down;
  env.peer_fail = mesh::PeerReachFailure::NoAnswer;
  env.peer_fails_in_row = 2;
  EXPECT_EQ(CallResultLine(kCall, gave_up, env).str(),
            "event=call.result call=ddc19c94 role=callee ok=0 attempts=1 ms=31000 kind=unknown family=0 err=unreachable "
            "reach_err=not_connected why=other relays=7 relays_up=2 peer_state=down peer_fail=no_answer peer_fails=2");
}

TEST(CallConnectDiagTest, RelayAndPunchLines) {
  const std::string relay = CallRelayLine("out", kCall, "QmXYY8waMbbuh4cpUXtKiwum8G6no8BKuEmCxFhS1hGwS1").str();
  EXPECT_EQ(relay.rfind("event=call.relay call=ddc19c94 dir=out relay=", 0), 0u);
  EXPECT_EQ(relay.size(), std::string("event=call.relay call=ddc19c94 dir=out relay=").size() + 8) << "a short ref, not the id";
  EXPECT_EQ(relay.find("QmXYY8wa"), std::string::npos);
  EXPECT_EQ(CallPunchLine("burst_fail", kCall, 3000, 4, 3010).str(),
            "event=call.punch call=ddc19c94 phase=burst_fail window_ms=3000 addrs=4 ms=3010");
}

// Every line of every event passes the bundle's last gate, so a send is never refused because of them.
TEST(CallConnectDiagTest, EveryLinePassesTheSelfCheck) {
  CallConnectEnv env;
  env.local = CountAddrs(IphoneAdvertised());
  CallResultFacts facts;
  facts.attempts = 3;
  const std::string all =
      CallNetLine(kCall, true, env).str() + "\n" + CallSignalLine("accept_in", kCall, IphoneAdvertised(), 5, false).str() +
      "\n" + CallReachOkLine(kCall, false, 1, mesh::PeerLinkKind::Relayed, true, 0, 7).str() + "\n" +
      CallRelayLine("in", kCall, "QmXYY8waMbbuh4cpUXtKiwum8G6no8BKuEmCxFhS1hGwS1").str() + "\n" +
      CallPunchLine("offer_in", kCall, 3000, 4, 0).str() + "\n" + CallResultLine(kCall, facts, env).str() + "\n";
  EXPECT_EQ(SelfCheck(all, {}), std::nullopt) << all;
}

TEST(CallDiagBudgetTest, CapsEachEventPerCallAndForgetsOldCalls) {
  CallDiagBudget budget;
  int allowed = 0;
  for (int i = 0; i < 100; ++i) {
    allowed += budget.Allow("call:aaaaaaaa11", CallDiagEvent::Reach) ? 1 : 0;
  }
  EXPECT_EQ(allowed, CallDiagBudget::kPerEventPerCall);
  EXPECT_TRUE(budget.Allow("call:aaaaaaaa11", CallDiagEvent::Result)) << "another event has its own room";
  EXPECT_TRUE(budget.Allow("call:bbbbbbbb22", CallDiagEvent::Reach)) << "another call has its own room";
  // More calls than it remembers: the oldest is forgotten, so memory stays bounded.
  for (size_t i = 0; i < CallDiagBudget::kCalls + 2; ++i) {
    EXPECT_TRUE(budget.Allow("call:c" + std::to_string(i) + "cccccccc", CallDiagEvent::Net));
  }
  EXPECT_TRUE(budget.Allow("call:aaaaaaaa11", CallDiagEvent::Reach)) << "its room started over";
}

}  // namespace
}  // namespace pbr
