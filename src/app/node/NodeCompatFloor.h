#pragma once

#include "common/Error.h"
#include "lib/net/ClientCompat.h"

#include <cstdint>
#include <functional>
#include <string>

namespace pbr {

/**
 * pp-node's reading of its relay's `client-compat` node floor (`min_node_version`; release-compatibility A2, RC018
 * part 1 item 4). Below the floor the relay answers this node's registration and heartbeats `426`, so the node stops
 * asking it — no registration, no heartbeat — and says so in one clear ERROR; the mesh services keep running (peers
 * that reach it directly are still served, and a supervisor does not restart it in a loop), until it is upgraded or
 * the floor is lowered. Read at start-up and again once a day (kRecheckMs); a fetch that fails keeps what was read
 * last, and none read yet is no floor (fail open: an unreachable relay must not stop a node it would take). Compared
 * by the version core, as the relay compares (NodeBelowFloor). Main-loop thread only.
 */
class NodeCompatFloor {
public:
  static constexpr int64_t kRecheckMs = 24LL * 60 * 60 * 1000;

  using Fetch = std::function<Roe<ClientCompatDocument>()>;

  /** `fetch`: the relay's client-compat (none: no floor is ever read); `steady_ms`: a monotonic clock. */
  NodeCompatFloor(Fetch fetch, std::string node_version, std::function<int64_t()> steady_ms);

  /** Reads the floor when due; false while this node is below it (one ERROR when it first finds so). */
  bool RelayCallsAllowed();
  /** The floor last read (empty: none). */
  const std::string& Floor() const { return floor_; }

  /** "this pp-node (X) is below the relay's min_node_version Y: …" (the ERROR's text). */
  static std::string BelowMessage(const std::string& node_version, const std::string& floor);

private:
  Fetch fetch_;
  std::string node_version_;
  std::function<int64_t()> steady_ms_;
  int64_t next_check_ms_ = INT64_MIN;
  bool below_ = false;
  std::string floor_;
};

} // namespace pbr
