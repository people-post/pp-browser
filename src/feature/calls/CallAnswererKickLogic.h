#pragma once

namespace pbr {

/** Gates for CallSessionManager::KickAnswererDirectMediaIfArmed (V037 / product glue). */
struct CallAnswererKickDecisionInput {
  bool allows_direct_path = false;
  /**
   * True only when duplex/capture is actually progressing for this call.
   * IsActive alone is insufficient — dogfood 6b68 StartSfu left active with tx_frames=0 and
   * Kick skipped forever while both sides stayed DirectConnecting.
   */
  bool media_live_same_call = false;
  bool peer_nonempty = false;
};

inline bool ShouldKickAnswererDirectMedia(const CallAnswererKickDecisionInput& in) {
  if (!in.allows_direct_path) {
    return false;
  }
  if (in.media_live_same_call) {
    return false;
  }
  return in.peer_nonempty;
}

} // namespace pbr
