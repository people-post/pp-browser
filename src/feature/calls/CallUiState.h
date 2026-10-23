#pragma once

#include "lib/calls/CallLifecycleTypes.h"
#include "lib/calls/CallMediaSeat.h"
#include "common/media/CallMediaHealth.h"

#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * What the GUI reads about the call stack, published by the calls owner after each of its steps
 * (projects/thread-ownership t2b): the GUI never reads owner memory directly.
 */
struct CallUiState {
  // Lifecycle
  CallPhase phase = CallPhase::Idle;
  CallMediaStatus media_status = CallMediaStatus::None;
  std::string active_call_id;
  /** When the shown call's media first went live (0: never) — the call timer, "was connected". */
  int64_t live_since_ms = 0;
  std::string accepting_call_id;
  std::string last_ring_call_id;
  std::string last_error;
  // Session manager / topology / bridge
  /** Why the hop path is being recovered (one reason; CallTopologyController::Recovery). */
  CallRecovery recovery = CallRecovery::None;
  bool p2p_connect_failed = false;
  bool p2p_connect_missing_mic = false;
  bool p2p_connect_seed_unreachable = false;
  std::string media_activity;
  /** Pending media error for the GUI to show once (TakeLastMediaError). */
  std::optional<std::string> last_media_error;
  /** The last call the peer ended or declined while this side had it (TakeRemoteEnd); empty if none. */
  std::string remote_ended_call_id;
  /** That end was the peer declining our call (else it ended the call). */
  bool remote_ended_declined = false;
  CallHopHealth hop_health;
  std::string media_path_kind;
  CallLinkCounters media_link;

  // Stack-level (hub / app reads from UI).
  bool available = false;              // sessions + lifecycle exist
  const void* sessions_identity = nullptr;  // changes when the session manager is rebuilt
  bool want_ephemeral_listen = false;  // N025 mobile listen desire
  bool connect_in_flight = false;      // call-media Connect sequence running

  bool MediaChromeLive() const {
    return phase == CallPhase::InCall &&
           (media_status == CallMediaStatus::DirectLive || media_status == CallMediaStatus::HopLive);
  }
  bool ShouldSuppressRing(const std::string& call_id) const {
    if (call_id.empty()) {
      return false;
    }
    return (!accepting_call_id.empty() && accepting_call_id == call_id) ||
           (phase == CallPhase::Accepting && active_call_id == call_id);
  }
};

} // namespace pbr
