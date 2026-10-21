// The 1:1 path's decisions (CallMediaBridge runs the outcomes).
#include "domain/calls/CallDirectPathLogic.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

DirectHealthInput Connecting() {
  DirectHealthInput in;
  in.engine_running = true;
  in.has_call = true;
  in.n_joined = 2;
  in.started_at_ms = 1000;
  in.now_ms = 2000;
  in.timeout_ms = 75000;
  return in;
}

TEST(CallDirectPathLogicTest, ConnectHealth) {
  DirectHealthInput in = Connecting();
  EXPECT_EQ(DecideDirectHealth(in), DirectHealthAction::None) << "still in time";
  in.now_ms = in.started_at_ms + in.timeout_ms;
  EXPECT_EQ(DecideDirectHealth(in), DirectHealthAction::TimedOut);

  DirectHealthInput dialing = in;
  dialing.connect_in_flight = true;
  EXPECT_EQ(DecideDirectHealth(dialing), DirectHealthAction::None) << "never time out a dial in progress";
  DirectHealthInput moving = in;
  moving.hop_in_flight = true;
  EXPECT_EQ(DecideDirectHealth(moving), DirectHealthAction::None) << "the hop owns it";
  DirectHealthInput group = in;
  group.n_joined = 3;
  EXPECT_EQ(DecideDirectHealth(group), DirectHealthAction::None) << "a group is the hop's to judge";
  DirectHealthInput failed = in;
  failed.already_failed = true;
  EXPECT_EQ(DecideDirectHealth(failed), DirectHealthAction::None) << "surfaced once";

  in.direct_ready = true;
  EXPECT_EQ(DecideDirectHealth(in), DirectHealthAction::HealConnected) << "the stream is up, the engine lags";
  in.engine_connected = true;
  EXPECT_EQ(DecideDirectHealth(in), DirectHealthAction::Healthy);
  in.has_call = false;
  EXPECT_EQ(DecideDirectHealth(in), DirectHealthAction::Healthy);
  in.engine_connected = false;
  EXPECT_EQ(DecideDirectHealth(in), DirectHealthAction::None) << "nothing to commit without a call";
}

TEST(CallDirectPathLogicTest, AFailedCarrier) {
  EXPECT_TRUE(IsSoftDirectClose("read_eof"));
  EXPECT_TRUE(IsSoftDirectClose("peer: stream closed"));
  EXPECT_FALSE(IsSoftDirectClose("timeout"));

  BundleFailureInput in;
  in.engine_call = true;
  in.engine_running = true;
  EXPECT_EQ(DecideBundleFailure(in), BundleFailureAction::Fail);
  in.soft_close = true;
  EXPECT_EQ(DecideBundleFailure(in), BundleFailureAction::Fail) << "a clean close before media connected";
  in.engine_connected = true;
  EXPECT_EQ(DecideBundleFailure(in), BundleFailureAction::AwaitHop) << "PreferLocal released the 1:1 stream";
  in.hop_attached = true;
  EXPECT_EQ(DecideBundleFailure(in), BundleFailureAction::CarriedByHop);
  in.direct_ready = true;
  EXPECT_EQ(DecideBundleFailure(in), BundleFailureAction::Ignore) << "another carrier has the stream";

  BundleFailureInput moving;
  moving.engine_call = true;
  moving.hop_pending = true;
  EXPECT_EQ(DecideBundleFailure(moving), BundleFailureAction::AwaitHop);
  moving.engine_call = false;
  EXPECT_EQ(DecideBundleFailure(moving), BundleFailureAction::Ignore) << "not the engine's call";
}

TEST(CallDirectPathLogicTest, NetworkChange) {
  EXPECT_EQ(DecideOnNetworkChange(CallDirectPlannerPhase::Reconnecting, false, false), NetworkChangeAction::Reanchor);
  EXPECT_EQ(DecideOnNetworkChange(CallDirectPlannerPhase::Live, true, true), NetworkChangeAction::RestartDirectUpgrade);
  EXPECT_EQ(DecideOnNetworkChange(CallDirectPlannerPhase::Live, true, false), NetworkChangeAction::None)
      << "the answerer follows";
  EXPECT_EQ(DecideOnNetworkChange(CallDirectPlannerPhase::Live, false, true), NetworkChangeAction::None)
      << "a direct path fails over by itself";
}

TEST(CallDirectPathLogicTest, UpgradeAndStandby) {
  PathKeepingInput in;
  in.offerer = true;
  in.has_call = true;
  in.on_relay = true;
  in.policy_upgrade_to_direct = true;
  in.policy_relay_standby = true;
  in.has_circuit_reach = true;
  EXPECT_TRUE(ShouldArmDirectUpgrade(in));
  EXPECT_FALSE(ShouldArmRelayStandby(in)) << "already on the relay";
  in.on_relay = false;
  EXPECT_FALSE(ShouldArmDirectUpgrade(in));
  EXPECT_TRUE(ShouldArmRelayStandby(in));
  in.has_circuit_reach = false;
  EXPECT_FALSE(ShouldArmRelayStandby(in));
  in.has_circuit_reach = true;
  in.already_for_call = true;
  EXPECT_FALSE(ShouldArmRelayStandby(in));
  in.already_for_call = false;
  in.offerer = false;
  EXPECT_FALSE(ShouldArmRelayStandby(in)) << "the offerer drives";

  EXPECT_EQ(RetryDelayMs(kRelayStandbyDelaysMs, 0), 5000);
  EXPECT_EQ(RetryDelayMs(kDirectUpgradeDelaysMs, 2), 60000);
  EXPECT_EQ(RetryDelayMs(kDirectUpgradeDelaysMs, 3), std::nullopt);
}

} // namespace
} // namespace pbr
