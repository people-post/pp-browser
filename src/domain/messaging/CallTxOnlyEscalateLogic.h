#pragma once

#include <cstdint>
#include <string>

namespace pbr {

/**
 * After DirectConnected: TX alive + RX=0 for this long → force circuit (V038).
 * The peer only starts sending once its CallAccept/BeginSession lands (relay inbox poll +
 * answerer seed park can add several seconds); match the offerer inbound grace (15 s) so a
 * healthy same-LAN direct leg is not torn down before the peer's capture starts.
 */
inline constexpr int64_t kCallTxOnlyEscalateGraceMs = 15000;
inline constexpr uint64_t kCallTxOnlyEscalateMinTxFrames = 80;

/** Pure gates for CallMediaBridge::MaybeEscalateTxOnlyDirect (unit-testable). */
struct CallTxOnlyEscalateDecisionInput {
  bool already_done = false;
  bool sfu_attached = false;
  bool stopping = false;
  std::string media_path_kind;
  bool has_circuit_reach = false;
  bool direct_active = false;
  std::string active_call_id;
  std::string media_call_id;
  uint64_t rx_audio_frames = 0;
  uint64_t tx_audio_frames = 0;
  int64_t direct_connected_at_ms = 0;
  int64_t now_ms = 0;
  bool peer_nonempty = false;
};

inline bool ShouldEscalateTxOnlyDirect(const CallTxOnlyEscalateDecisionInput& in) {
  if (in.already_done || in.sfu_attached || in.stopping) {
    return false;
  }
  if (in.media_path_kind == "circuit") {
    return false;
  }
  if (!in.has_circuit_reach || !in.direct_active) {
    return false;
  }
  if (in.active_call_id.empty() || in.media_call_id != in.active_call_id) {
    return false;
  }
  if (in.rx_audio_frames > 0) {
    return false;
  }
  if (in.tx_audio_frames < kCallTxOnlyEscalateMinTxFrames) {
    return false;
  }
  if (in.direct_connected_at_ms <= 0 ||
      in.now_ms - in.direct_connected_at_ms < kCallTxOnlyEscalateGraceMs) {
    return false;
  }
  return in.peer_nonempty;
}

} // namespace pbr
