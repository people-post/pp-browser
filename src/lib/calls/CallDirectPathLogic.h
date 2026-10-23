#pragma once

#include "lib/calls/CallDirectPlannerLogic.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace pbr {

/**
 * Pure decisions of the 1:1 path (CallMediaBridge): its connect health, a carrier (bundle) that
 * failed, the device's network changing, and when to keep a relayed standby or try for a direct
 * link. The bridge gathers the facts (engine, transport, reach) and runs the outcome.
 */

// --- Connect health (the ~1 s tick while Connecting / Live / Degraded) ---------------------------

struct DirectHealthInput {
  /** A connect failure is already surfaced. */
  bool already_failed = false;
  /** The engine runs, in the packet (SFU-style) mode both paths use. */
  bool engine_running = false;
  /** A hop attach / migration owns the call's media for now. */
  bool hop_in_flight = false;
  /** A connect attempt is dialing (worker thread): never time it out from here. */
  bool connect_in_flight = false;
  bool engine_connected = false;
  /** The direct stream is up (MediaReady). */
  bool direct_ready = false;
  bool has_call = false;
  size_t n_joined = 0;
  /** When the engine started (Unix ms; 0: unknown). */
  int64_t started_at_ms = 0;
  int64_t now_ms = 0;
  int64_t timeout_ms = 0;
};

enum class DirectHealthAction {
  /** Nothing to judge (not ours, failed already, or still in time). */
  None,
  /** Connected and the stream is up: clear a stale failure, check for TX-only. */
  Healthy,
  /** The stream is up but the engine still says connecting: commit connected. */
  HealConnected,
  /** No stream within the connect budget: the call fails (stays open). */
  TimedOut,
};

DirectHealthAction DecideDirectHealth(const DirectHealthInput& in);

// --- A carrier failed ------------------------------------------------------------------------------

struct BundleFailureInput {
  /** The failed carrier is the engine's call. */
  bool engine_call = false;
  bool engine_running = false;
  bool engine_connected = false;
  /** Another carrier already brought the direct stream up. */
  bool direct_ready = false;
  /** The hop carries the media now (SoftMigrate swapped the send). */
  bool hop_attached = false;
  /** A hop attach is in flight or expected (the 1:1 stream closing is part of the move). */
  bool hop_pending = false;
  /** The peer closed the 1:1 stream (CallMediaErr::StreamClosed): a clean end, not a network failure. */
  bool soft_close = false;
};

enum class BundleFailureAction {
  /** A late failure of a carrier nobody uses. */
  Ignore,
  /** The hop carries the call: drop the 1:1 transport, the call is connected. */
  CarriedByHop,
  /** The call is moving to a hop: drop the 1:1 transport and wait for the attach. */
  AwaitHop,
  /** The call's media failed (it stays open: Retry / the peer's reconnect resumes it). */
  Fail,
};

BundleFailureAction DecideBundleFailure(const BundleFailureInput& in);

// --- k5: the device's network changed ------------------------------------------------------------

enum class NetworkChangeAction {
  None,
  /** Reconnecting: the new network is the best chance — re-anchor once the links settle. */
  Reanchor,
  /** Live on the relay: the direct upgrade starts over from the new network. */
  RestartDirectUpgrade,
};

NetworkChangeAction DecideOnNetworkChange(CallDirectPlannerPhase phase, bool on_relay, bool offerer);

// --- k3 / k6: try for a direct link; keep a relayed standby ---------------------------------------

struct PathKeepingInput {
  /** The offerer drives migrations; the answerer's transport follows. */
  bool offerer = false;
  bool has_call = false;
  /** The call rides a relay carrier now. */
  bool on_relay = false;
  /** Already scheduled for this call. */
  bool already_for_call = false;
  /** A circuit can be reached at all (standby only). */
  bool has_circuit_reach = false;
  /** The pair's path policy (k6). */
  bool policy_upgrade_to_direct = false;
  bool policy_relay_standby = false;
};

/** k3: a relayed call keeps trying for a direct link. */
bool ShouldArmDirectUpgrade(const PathKeepingInput& in);
/** k6: a direct call keeps a relayed standby. */
bool ShouldArmRelayStandby(const PathKeepingInput& in);

inline constexpr int kDirectUpgradeDelaysMs[] = {3000, 20000, 60000};
inline constexpr int kRelayStandbyDelaysMs[] = {5000, 20000, 60000};

/** The delay before attempt `attempt` (0-based) from `delays`; none once they run out. */
std::optional<int> RetryDelayMs(std::span<const int> delays, int attempt);

} // namespace pbr
