#pragma once

#include "domain/messaging/CallLifecycleTypes.h"
#include "feature/calls/CallMediaSeat.h"
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
  std::string accepting_call_id;
  std::string last_ring_call_id;
  std::string last_error;
  // Session manager / topology / bridge
  bool awaiting_sfu_recovery = false;
  bool soft_migrate_in_flight = false;
  bool sfu_attach_wait_active = false;
  bool p2p_connect_failed = false;
  bool p2p_connect_missing_mic = false;
  std::string media_activity;
  /** Pending media error for the GUI to show once (TakeLastMediaError). */
  std::optional<std::string> last_media_error;
  CallHopHealth hop_health;
  std::string media_path_kind;
  // Seat
  std::string seat_bound_call_id;
  CallMediaSeat::MediaState seat_state = CallMediaSeat::MediaState::Idle;
  bool seat_live = false;

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
  CallMediaSeat::MediaState SeatStateFor(const std::string& call_id) const {
    return !call_id.empty() && call_id == seat_bound_call_id ? seat_state : CallMediaSeat::MediaState::Idle;
  }
  bool SeatLiveFor(const std::string& call_id) const {
    return !call_id.empty() && call_id == seat_bound_call_id && seat_live;
  }
};

} // namespace pbr
