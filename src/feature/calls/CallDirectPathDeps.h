#pragma once

#include "domain/mesh/l4/call_media/ICallMediaTransport.h"
#include "domain/mesh/reach/PeerReachCoordinator.h"
#include "domain/messaging/CallPathPolicy.h"
#include "feature/calls/CallTopologyRelayDeps.h"

#include <functional>
#include <memory>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * What the 1:1 path runs on, handed to the session manager (which owns the path) when mesh media is
 * wired: the call-media transport and the mesh reach it dials through (not owned), the rendezvous
 * seed hooks, and this pair's path policy.
 */
struct CallDirectPathDeps {
  ICallMediaTransport* transport = nullptr;
  IDialRegistry* dial = nullptr;
  ICircuitHopReach* circuit_reach = nullptr;
  /** Where reach runs: the mesh's io lane (a test's lane on the test path). */
  std::shared_ptr<pp::SerialExecutor> lane;
  std::function<void()> seed_warm;
  std::function<void()> seed_reserve;
  std::function<void(std::function<void(bool)> done, int timeout_ms)> seed_park_await;
  std::function<CallPathPolicy(const std::string& call_id)> path_policy;

  bool IsUsable() const { return transport && dial; }
  PeerReachPorts Reach() const { return {dial, circuit_reach, lane}; }
};

} // namespace pbr
