#include "domain/calls/CallDirectPathLogic.h"

#include "domain/calls/CallMediaPlannerSelectLogic.h"

#include "common/PbrCompat.h"

namespace pbr {

DirectHealthAction DecideDirectHealth(const DirectHealthInput& in) {
  if (in.already_failed || !in.engine_running || in.hop_in_flight || in.connect_in_flight) {
    return DirectHealthAction::None;
  }
  if (in.direct_ready && in.engine_connected) {
    return DirectHealthAction::Healthy;
  }
  if (!in.has_call) {
    return DirectHealthAction::None;
  }
  if (in.direct_ready) {
    return DirectHealthAction::HealConnected;  // StartSfu's "connecting" left the engine behind
  }
  if (ShouldArmHopPlanner(in.n_joined) || in.started_at_ms <= 0) {
    return DirectHealthAction::None;  // a group call's media is the hop's to judge
  }
  return in.now_ms - in.started_at_ms >= in.timeout_ms ? DirectHealthAction::TimedOut : DirectHealthAction::None;
}

bool IsSoftDirectClose(const std::string& reason) {
  return reason.find("read_eof") != std::string::npos || reason.find("stream closed") != std::string::npos;
}

BundleFailureAction DecideBundleFailure(const BundleFailureInput& in) {
  if (!in.engine_call || (in.direct_ready && in.engine_connected)) {
    return BundleFailureAction::Ignore;
  }
  // SoftMigrate swaps the send to the hop but leaves the 1:1 stream until it is released: its
  // teardown must not fail a call the hop carries.
  if (in.hop_attached && in.engine_connected) {
    return BundleFailureAction::CarriedByHop;
  }
  // PreferLocal releases the 1:1 stream while capture stays up and the attach may still lag.
  if (in.hop_pending || (in.soft_close && in.engine_running && in.engine_connected)) {
    return BundleFailureAction::AwaitHop;
  }
  return BundleFailureAction::Fail;
}

NetworkChangeAction DecideOnNetworkChange(const CallDirectPlannerPhase phase, const bool on_relay,
                                          const bool offerer) {
  if (phase == CallDirectPlannerPhase::Reconnecting) {
    return NetworkChangeAction::Reanchor;
  }
  // Live on a direct path: if it died with the old network, the transport fails over to the
  // standby or reports the path lost.
  if (phase == CallDirectPlannerPhase::Live && on_relay && offerer) {
    return NetworkChangeAction::RestartDirectUpgrade;
  }
  return NetworkChangeAction::None;
}

bool ShouldArmDirectUpgrade(const PathKeepingInput& in) {
  return in.offerer && in.has_call && in.on_relay && !in.already_for_call && in.policy_upgrade_to_direct;
}

bool ShouldArmRelayStandby(const PathKeepingInput& in) {
  return in.offerer && in.has_call && !in.on_relay && !in.already_for_call && in.has_circuit_reach &&
         in.policy_relay_standby;
}

std::optional<int> RetryDelayMs(const std::span<const int> delays, const int attempt) {
  if (attempt < 0 || static_cast<size_t>(attempt) >= delays.size()) {
    return std::nullopt;
  }
  return delays[static_cast<size_t>(attempt)];
}

} // namespace pbr
